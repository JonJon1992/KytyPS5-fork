// Included in spirvEmitterBvh.cpp's anonymous namespace. This diagnostic
// copies through the GPU page table at execution time, never CPU-after backing.
uint32_t AstroCapturePointer(EmitterState& s, uint32_t word) {
	const auto pointer = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpAccessChain,TypeStorageBufferElementPointer(s),pointer,
	    s.fault_buffer_variable,ConstantU32(s,0),word);
	return pointer;
}

uint32_t CopyAstroGuestWords(EmitterState& s, uint32_t guest, uint32_t words, uint32_t destination) {
	const auto u=TypeU32(s), b=TypeBool(s), wide=TypeScalarU64(s);
	const auto pre=s.current_label;
	const auto header=s.builder.AllocateId(), body=s.builder.AllocateId();
	const auto next=s.builder.AllocateId(), merge=s.builder.AllocateId();
	const auto index=s.builder.AllocateId(), next_index=s.builder.AllocateId();
	const auto all=s.builder.AllocateId(), next_all=s.builder.AllocateId();
	s.builder.AddFunction(spv::OpBranch,header);
	EmitLabel(s,header);
	s.builder.AddFunction(spv::OpPhi,u,index,ConstantU32(s,0),pre,next_index,next);
	s.builder.AddFunction(spv::OpPhi,b,all,ConstantBool(s,true),pre,next_all,next);
	const auto active=Binary(s,spv::OpULessThan,b,index,words);
	s.builder.AddFunction(spv::OpLoopMerge,merge,next,spv::LoopControlMaskNone);
	s.builder.AddFunction(spv::OpBranchConditional,active,body,merge);
	EmitLabel(s,body);
	const auto address=Binary(s,spv::OpIAdd,wide,guest,
	    Unary(s,spv::OpUConvert,wide,Binary(s,spv::OpShiftLeftLogical,u,index,ConstantU32(s,2))));
	const auto host=GetBdaPointer(s,address);
	const auto present=Binary(s,spv::OpINotEqual,b,host,ConstantDeviceAddress(s,0));
	const auto value=EmitValueOrDefaultIfCondition(s,present,u,ConstantU32(s,0),[&] {
		return NodeWord(s,host,ConstantU32(s,0));
	});
	s.builder.AddFunction(spv::OpStore,
	    AstroCapturePointer(s,Binary(s,spv::OpIAdd,u,destination,index)),value);
	s.builder.AddFunction(spv::OpLogicalAnd,b,next_all,all,present);
	s.builder.AddFunction(spv::OpBranch,next);
	EmitLabel(s,next);
	s.builder.AddFunction(spv::OpIAdd,u,next_index,index,ConstantU32(s,1));
	s.builder.AddFunction(spv::OpBranch,header);
	EmitLabel(s,merge);
	return all;
}

void CaptureAstroScene(EmitterState& s, uint32_t descriptor, uint32_t context, uint32_t slot, uint32_t pc) {
	namespace C=BvhCapture;
	const auto u=TypeU32(s), b=TypeBool(s), wide=TypeScalarU64(s);
	(void)EmitValueOrDefaultIfCondition(s,
	    Binary(s,spv::OpULessThan,b,slot,ConstantU32(s,C::MaxScenes)),u,ConstantU32(s,0),[&] {
		constexpr auto base=uint32_t(BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE/4);
		const auto at=Binary(s,spv::OpIAdd,u,ConstantU32(s,base+C::SceneOffset),
		    Binary(s,spv::OpIMul,u,slot,ConstantU32(s,C::SceneWords)));
		const auto destination=[&](uint32_t field) { return Binary(s,spv::OpIAdd,u,at,ConstantU32(s,field)); };
		const auto store=[&](uint32_t field,uint32_t value) {
			s.builder.AddFunction(spv::OpStore,AstroCapturePointer(s,destination(field)),value);
		};
		const auto inst=DeviceAddressFromWords(s,Extract(s,u,context,0),Extract(s,u,context,1));
		const auto blas=Binary(s,spv::OpShiftLeftLogical,wide,
		    DeviceAddressFromWords(s,Extract(s,u,descriptor,0),
		        Binary(s,spv::OpBitwiseAnd,u,Extract(s,u,descriptor,1),ConstantU32(s,255))),
		    ConstantDeviceAddress(s,8));
		store(C::SceneStatus,ConstantU32(s,C::InvalidScene));
		store(C::SceneVersion,ConstantU32(s,1));
		store(C::BlasLow,Unary(s,spv::OpUConvert,u,blas));
		store(C::BlasHigh,Unary(s,spv::OpUConvert,u,
		    Binary(s,spv::OpShiftRightLogical,wide,blas,ConstantDeviceAddress(s,32))));
		for(uint32_t i=0;i<4;++i)store(C::SceneInstanceLow+i,Extract(s,u,context,i));
		store(C::ScenePc,pc);
		auto valid=Binary(s,spv::OpINotEqual,b,inst,ConstantDeviceAddress(s,0));
		valid=Binary(s,spv::OpLogicalAnd,b,valid,
		    Binary(s,spv::OpULessThanEqual,b,inst,ConstantDeviceAddress(s,(1ull<<40)-160)));
		valid=Binary(s,spv::OpLogicalAnd,b,valid,
		    Binary(s,spv::OpIEqual,b,
		        Binary(s,spv::OpBitwiseAnd,u,Extract(s,u,context,0),ConstantU32(s,3)),ConstantU32(s,0)));
		(void)EmitValueOrDefaultIfCondition(s,valid,u,ConstantU32(s,0),[&] {
			store(C::SceneStatus,ConstantU32(s,C::MissingScene));
			const auto instance_present=CopyAstroGuestWords(s,inst,ConstantU32(s,40),destination(C::InstanceData));
			const auto header=GetBdaPointer(s,Binary(s,spv::OpIAdd,wide,blas,ConstantDeviceAddress(s,16)));
			(void)EmitValueOrDefaultIfCondition(s,
			    Binary(s,spv::OpINotEqual,b,header,ConstantDeviceAddress(s,0)),u,ConstantU32(s,0),[&] {
				const auto bytes=NodeWord(s,header,ConstantU32(s,0));
				const auto high=NodeWord(s,header,ConstantU32(s,1));
				store(C::BlasBytes,bytes);
				store(C::SceneStatus,ConstantU32(s,C::LargeScene));
				auto bounded=Binary(s,spv::OpIEqual,b,high,ConstantU32(s,0));
				bounded=Binary(s,spv::OpLogicalAnd,b,bounded,
				    Binary(s,spv::OpULessThanEqual,b,bytes,ConstantU32(s,1024)));
				bounded=Binary(s,spv::OpLogicalAnd,b,bounded,
				    Binary(s,spv::OpUGreaterThanEqual,b,bytes,ConstantU32(s,96)));
				bounded=Binary(s,spv::OpLogicalAnd,b,bounded,
				    Binary(s,spv::OpIEqual,b,Binary(s,spv::OpBitwiseAnd,u,bytes,ConstantU32(s,3)),ConstantU32(s,0)));
				bounded=Binary(s,spv::OpLogicalAnd,b,bounded,
				    Binary(s,spv::OpULessThanEqual,b,blas,ConstantDeviceAddress(s,(1ull<<40)-1024)));
				(void)EmitValueOrDefaultIfCondition(s,bounded,u,ConstantU32(s,0),[&] {
					const auto copied=CopyAstroGuestWords(s,blas,
					    Binary(s,spv::OpShiftRightLogical,u,bytes,ConstantU32(s,2)),destination(C::BlasData));
					const auto complete=Binary(s,spv::OpLogicalAnd,b,instance_present,copied);
					store(C::SceneStatus,Select(s,u,complete,ConstantU32(s,C::CompleteScene),ConstantU32(s,C::MissingScene)));
					return ConstantU32(s,0);
				});
				return ConstantU32(s,0);
			});
			return ConstantU32(s,0);
		});
		return ConstantU32(s,0);
	});
}
