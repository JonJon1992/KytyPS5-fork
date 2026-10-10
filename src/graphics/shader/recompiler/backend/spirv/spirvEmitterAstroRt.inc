// Native BLAS traversal for a checked, immutable single-list Astro scene.
// Reuse the software triangle arithmetic for guest facing, distance and IDs.
namespace {
uint32_t AstroRtWord(EmitterState& s,uint32_t index) {
  constexpr auto base=uint32_t((BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE+BvhCapture::Bytes)/4);
  auto p=AstroCapturePointer(s,Binary(s,spv::OpIAdd,TypeU32(s),ConstantU32(s,base),index));
  const auto value=s.builder.AllocateId();s.builder.AddFunction(spv::OpLoad,TypeU32(s),value,p);return value;
}
uint32_t AstroCurrentBytesMatch(EmitterState& s,uint32_t guest,uint32_t count) {
  namespace N=AstroNativeBinding;
  const auto u=TypeU32(s),b=TypeBool(s),wide=TypeScalarU64(s);
  const auto pre=s.current_label,header=s.builder.AllocateId(),body=s.builder.AllocateId();
  const auto next=s.builder.AllocateId(),merge=s.builder.AllocateId();
  const auto index=s.builder.AllocateId(),next_index=s.builder.AllocateId();
  const auto valid=s.builder.AllocateId(),next_valid=s.builder.AllocateId();
  s.builder.AddFunction(spv::OpBranch,header);EmitLabel(s,header);
  s.builder.AddFunction(spv::OpPhi,u,index,ConstantU32(s,0),pre,next_index,next);
  s.builder.AddFunction(spv::OpPhi,b,valid,ConstantBool(s,true),pre,next_valid,next);
  const auto work=Binary(s,spv::OpLogicalAnd,b,valid,Binary(s,spv::OpULessThan,b,index,count));
  s.builder.AddFunction(spv::OpLoopMerge,merge,next,spv::LoopControlMaskNone);
  s.builder.AddFunction(spv::OpBranchConditional,work,body,merge);EmitLabel(s,body);
  const auto offset=Unary(s,spv::OpUConvert,wide,Binary(s,spv::OpShiftLeftLogical,u,index,ConstantU32(s,2)));
  const auto pointer=GetBdaPointer(s,Binary(s,spv::OpIAdd,wide,guest,offset));
  const auto present=Binary(s,spv::OpINotEqual,b,pointer,ConstantDeviceAddress(s,0));
  const auto value=EmitValueOrDefaultIfCondition(s,present,u,ConstantU32(s,0),[&]{return NodeWord(s,pointer,ConstantU32(s,0));});
  const auto equal=Binary(s,spv::OpIEqual,b,value,AstroRtWord(s,Binary(s,spv::OpIAdd,u,ConstantU32(s,N::Data),index)));
  s.builder.AddFunction(spv::OpLogicalAnd,b,next_valid,present,equal);
  s.builder.AddFunction(spv::OpBranch,next);EmitLabel(s,next);
  s.builder.AddFunction(spv::OpIAdd,u,next_index,index,ConstantU32(s,1));
  s.builder.AddFunction(spv::OpBranch,header);EmitLabel(s,merge);return valid;
}
}

void DefineAstroTrace(EmitterState& s) {
  if(!s.requirements.astro_rt)return;
  namespace N=AstroNativeBinding;
  s.builder.RequireCapability(spv::CapabilityRayQueryKHR);
  s.builder.RequireExtension("SPV_KHR_ray_query");
  const auto u=TypeU32(s),b=TypeBool(s),f=TypeF32(s),vec4=TypeU32Vector(s,4);
  const auto query_type=s.builder.Type(spv::OpTypeRayQueryKHR);
  const auto query_ptr=s.builder.Type(spv::OpTypePointer,spv::StorageClassFunction,query_type);
  const auto best_ptr=s.builder.Type(spv::OpTypePointer,spv::StorageClassFunction,vec4);
  const auto bad_ptr=s.builder.Type(spv::OpTypePointer,spv::StorageClassFunction,b);
  const auto index_ptr=s.builder.Type(spv::OpTypePointer,spv::StorageClassFunction,u);
  const auto signature=s.builder.Type(spv::OpTypeFunction,vec4,vec4,vec4,vec4,u);
  s.astro_trace_function=s.builder.AllocateId();s.builder.AddName(s.astro_trace_function,"astro_native_blas");
  s.builder.AddFunction(spv::OpFunction,vec4,s.astro_trace_function,spv::FunctionControlMaskNone,signature);
  std::array<uint32_t,4> args{};
  for(uint32_t i=0;i<4;i++) {args[i]=s.builder.AllocateId();s.builder.AddFunction(spv::OpFunctionParameter,i==3?u:vec4,args[i]);}
  EmitLabel(s,s.builder.AllocateId());
  const auto query=s.builder.AllocateId(),best=s.builder.AllocateId(),bad=s.builder.AllocateId(),best_index=s.builder.AllocateId();
  s.builder.AddFunction(spv::OpVariable,query_ptr,query,spv::StorageClassFunction);
  s.builder.AddFunction(spv::OpVariable,best_ptr,best,spv::StorageClassFunction);
  s.builder.AddFunction(spv::OpVariable,bad_ptr,bad,spv::StorageClassFunction);
  s.builder.AddFunction(spv::OpVariable,index_ptr,best_index,spv::StorageClassFunction);
  const auto word=[&](uint32_t i){return AstroRtWord(s,ConstantU32(s,i));};
  const auto both=[&](uint32_t a,uint32_t c){return Binary(s,spv::OpLogicalAnd,b,a,c);};
  const auto equal=[&](uint32_t a,uint32_t c){return Binary(s,spv::OpIEqual,b,a,c);};
  const auto load=[&](uint32_t type,uint32_t ptr){auto v=s.builder.AllocateId();s.builder.AddFunction(spv::OpLoad,type,v,ptr);return v;};
  const auto source=DeviceAddressFromWords(s,word(N::BaseLow),word(N::BaseHigh));
  const auto native=DeviceAddressFromWords(s,word(N::TlasLow),word(N::TlasHigh));
  const auto bytes=word(N::ByteCount),primitive_count=word(N::PrimitiveCount);
  auto valid=both(equal(word(N::Magic),ConstantU32(s,N::MagicValue)),equal(word(N::Version),ConstantU32(s,N::VersionValue)));
  valid=both(valid,Binary(s,spv::OpINotEqual,b,native,ConstantDeviceAddress(s,0)));
  valid=both(valid,Binary(s,spv::OpUGreaterThanEqual,b,bytes,ConstantU32(s,96)));
  valid=both(valid,Binary(s,spv::OpULessThanEqual,b,bytes,ConstantU32(s,1024)));
  valid=both(valid,equal(Binary(s,spv::OpBitwiseAnd,u,bytes,ConstantU32(s,3)),ConstantU32(s,0)));
  valid=both(valid,Binary(s,spv::OpULessThanEqual,b,source,ConstantDeviceAddress(s,(1ull<<40)-1024)));
  valid=both(valid,Binary(s,spv::OpUGreaterThan,b,primitive_count,ConstantU32(s,0)));
  valid=both(valid,Binary(s,spv::OpULessThanEqual,b,primitive_count,ConstantU32(s,N::MaxPrimitives)));
  valid=both(valid,equal(Extract(s,u,args[2],3),word(N::Root)));
  for(uint32_t i=0;i<4;i++)valid=both(valid,equal(Extract(s,u,args[0],i),word(N::Descriptor+i)));
  Vec3 origin{},direction{},inverse{};
  for(uint32_t i=0;i<3;i++) {
    origin[i]=Unary(s,spv::OpBitcast,f,Extract(s,u,args[1],i));
    direction[i]=Unary(s,spv::OpBitcast,f,Extract(s,u,args[2],i));
    inverse[i]=Binary(s,spv::OpFDiv,f,ConstantF32Value(s,1),direction[i]);
    for(auto x:{origin[i],direction[i]})valid=both(valid,Unary(s,spv::OpLogicalNot,b,
      Binary(s,spv::OpLogicalOr,b,Unary(s,spv::OpIsNan,b,x),Unary(s,spv::OpIsInf,b,x))));
  }
  auto nonzero=ConstantBool(s,false);
  for(auto x:direction)nonzero=Binary(s,spv::OpLogicalOr,b,nonzero,Binary(s,spv::OpFOrdNotEqual,b,x,ConstantF32Value(s,0)));
  valid=both(valid,nonzero);
  const auto extent=Unary(s,spv::OpBitcast,f,Extract(s,u,args[1],3));
  valid=both(valid,Binary(s,spv::OpFOrdGreaterThan,b,extent,ConstantF32Value(s,0)));
  valid=both(valid,Unary(s,spv::OpLogicalNot,b,Unary(s,spv::OpIsInf,b,extent)));
  const auto result=EmitValueOrDefaultIfCondition(s,valid,vec4,ConstantU32CompositeZero(s,4),[&] {
    const auto matches=AstroCurrentBytesMatch(s,source,Binary(s,spv::OpShiftRightLogical,u,bytes,ConstantU32(s,2)));
    return EmitValueOrDefaultIfCondition(s,matches,vec4,ConstantU32CompositeZero(s,4),[&] {
      const auto root=word(N::Root);
      const auto at=Binary(s,spv::OpIAdd,TypeScalarU64(s),source,Unary(s,spv::OpUConvert,TypeScalarU64(s),
        Binary(s,spv::OpShiftLeftLogical,u,Binary(s,spv::OpShiftRightLogical,u,root,ConstantU32(s,3)),ConstantU32(s,6))));
      const auto first=GetBdaPointer(s,at),second=GetBdaPointer(s,Binary(s,spv::OpIAdd,TypeScalarU64(s),at,ConstantDeviceAddress(s,64)));
      const auto children=Boxes(s,first,second,equal(Binary(s,spv::OpBitwiseAnd,u,root,ConstantU32(s,7)),ConstantU32(s,4)),
                               ConstantU32(s,0),ConstantBool(s,true),extent,origin,inverse);
      auto admitted=ConstantBool(s,false);
      for(uint32_t i=0;i<4;i++)admitted=Binary(s,spv::OpLogicalOr,b,admitted,
        Binary(s,spv::OpINotEqual,b,Extract(s,u,children,i),ConstantU32(s,~0u)));
      return EmitValueOrDefaultIfCondition(s,admitted,vec4,ConstantU32CompositeZero(s,4),[&] {
        s.builder.AddFunction(spv::OpStore,best,Vector(s,vec4,std::array{Unary(s,spv::OpBitcast,u,extent),ConstantU32(s,~0u),ConstantU32(s,~0u),ConstantU32(s,0)}));
        s.builder.AddFunction(spv::OpStore,bad,ConstantBool(s,false));
        s.builder.AddFunction(spv::OpStore,best_index,ConstantU32(s,0));
        const auto acceleration=Unary(s,spv::OpConvertUToAccelerationStructureKHR,s.builder.Type(spv::OpTypeAccelerationStructureKHR),native);
        // Do not confirm candidates: native traversal order and equal-distance
        // commitment must not choose the game's last entry in the list.
        const auto padding=Binary(s,spv::OpFMul,f,ConstantF32Value(s,2e-5f),
          Binary(s,spv::OpFAdd,f,extent,ConstantF32Value(s,1)));
        s.builder.AddFunction(spv::OpRayQueryInitializeKHR,query,acceleration,ConstantU32(s,2),ConstantU32(s,255),
          Vector(s,TypeF32Vector(s,3),origin),ConstantF32Value(s,0),Vector(s,TypeF32Vector(s,3),direction),
          Binary(s,spv::OpFAdd,f,extent,padding));
        const auto header=s.builder.AllocateId(),body=s.builder.AllocateId(),next=s.builder.AllocateId(),merge=s.builder.AllocateId();
        s.builder.AddFunction(spv::OpBranch,header);EmitLabel(s,header);
        const auto proceed=s.builder.AllocateId();s.builder.AddFunction(spv::OpRayQueryProceedKHR,b,proceed,query);
        s.builder.AddFunction(spv::OpLoopMerge,merge,next,spv::LoopControlMaskNone);
        s.builder.AddFunction(spv::OpBranchConditional,proceed,body,merge);EmitLabel(s,body);
        const auto candidate=s.builder.AllocateId();
        s.builder.AddFunction(spv::OpRayQueryGetIntersectionPrimitiveIndexKHR,u,candidate,query,ConstantU32(s,0));
        (void)EmitValueOrDefaultIfCondition(s,Binary(s,spv::OpULessThan,b,candidate,primitive_count),u,ConstantU32(s,0),[&] {
          const auto bary=s.builder.AllocateId();
          s.builder.AddFunction(spv::OpRayQueryGetIntersectionBarycentricsKHR,TypeF32Vector(s,2),bary,query,ConstantU32(s,0));
          const auto x=Extract(s,f,bary,0),y=Extract(s,f,bary,1);
          const auto z=Binary(s,spv::OpFSub,f,Binary(s,spv::OpFSub,f,ConstantF32Value(s,1),x),y);
          auto edge=ConstantBool(s,false);
          for(auto v:{x,y,z})edge=Binary(s,spv::OpLogicalOr,b,edge,Binary(s,spv::OpFOrdLessThanEqual,b,
            EmitGlsl<GLSLstd450FAbs,IR::Type::F32>(s,v),ConstantF32Value(s,1e-5f)));
          s.builder.AddFunction(spv::OpStore,bad,Binary(s,spv::OpLogicalOr,b,load(b,bad),edge));
          const auto metadata=Binary(s,spv::OpIAdd,u,ConstantU32(s,N::Primitives),Binary(s,spv::OpIMul,u,candidate,ConstantU32(s,2)));
          const auto node=AstroRtWord(s,metadata),leaf=AstroRtWord(s,Binary(s,spv::OpIAdd,u,metadata,ConstantU32(s,1)));
          const auto address=Binary(s,spv::OpIAdd,TypeScalarU64(s),source,Unary(s,spv::OpUConvert,TypeScalarU64(s),
            Binary(s,spv::OpShiftLeftLogical,u,Binary(s,spv::OpShiftRightLogical,u,node,ConstantU32(s,3)),ConstantU32(s,6))));
          const auto tri=Triangle(s,GetBdaPointer(s,address),Binary(s,spv::OpBitwiseAnd,u,node,ConstantU32(s,7)),ConstantBool(s,true),origin,direction);
          const auto numerator=Extract(s,u,tri,0),denominator=Extract(s,u,tri,1);
          const auto t=Binary(s,spv::OpFMul,f,Unary(s,spv::OpBitcast,f,numerator),
            Binary(s,spv::OpFDiv,f,ConstantF32Value(s,1),Unary(s,spv::OpBitcast,f,denominator)));
          s.builder.AddAnnotation(spv::OpDecorate,t,spv::DecorationNoContraction);
          const auto old=load(vec4,best),old_t=Unary(s,spv::OpBitcast,f,Extract(s,u,old,0));
          auto accepted=Binary(s,spv::OpINotEqual,b,numerator,ConstantU32(s,0x07f80000));
          const auto two_sided=Binary(s,spv::OpINotEqual,b,Binary(s,spv::OpBitwiseAnd,u,args[3],ConstantU32(s,4)),ConstantU32(s,0));
          const auto facing=equal(Binary(s,spv::OpShiftRightLogical,u,numerator,ConstantU32(s,31)),
            Binary(s,spv::OpBitwiseAnd,u,Binary(s,spv::OpShiftRightLogical,u,args[3],ConstantU32(s,3)),ConstantU32(s,1)));
          accepted=both(accepted,Binary(s,spv::OpLogicalOr,b,two_sided,facing));
          accepted=both(accepted,Binary(s,spv::OpFOrdLessThanEqual,b,t,old_t));
          const auto tie=Binary(s,spv::OpLogicalOr,b,Binary(s,spv::OpFOrdLessThan,b,t,old_t),
            Binary(s,spv::OpUGreaterThanEqual,b,candidate,load(u,best_index)));
          accepted=both(accepted,tie);
          (void)EmitValueOrDefaultIfCondition(s,accepted,u,ConstantU32(s,0),[&] {
            s.builder.AddFunction(spv::OpStore,best,Vector(s,vec4,std::array{Unary(s,spv::OpBitcast,u,t),node,leaf,ConstantU32(s,1)}));
            s.builder.AddFunction(spv::OpStore,best_index,candidate);return ConstantU32(s,0);
          });return ConstantU32(s,0);
        });
        s.builder.AddFunction(spv::OpBranch,next);EmitLabel(s,next);s.builder.AddFunction(spv::OpBranch,header);EmitLabel(s,merge);
        const auto hit=load(vec4,best);
        auto usable=both(Unary(s,spv::OpLogicalNot,b,load(b,bad)),
          Binary(s,spv::OpINotEqual,b,Extract(s,u,hit,1),ConstantU32(s,~0u)));
        // Zero-distance/sign and native misses are deliberately delegated to
        // software. This is a per-lane fallback, not a fabricated intersection.
        usable=both(usable,Binary(s,spv::OpFOrdGreaterThan,b,Unary(s,spv::OpBitcast,f,Extract(s,u,hit,0)),ConstantF32Value(s,1e-5f)));
        return EmitValueOrDefaultIfCondition(s,usable,vec4,ConstantU32CompositeZero(s,4),[&]{return hit;});
      });
    });
  });
  constexpr auto base=uint32_t((BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE+BvhCapture::Bytes)/4);
  const auto used=Binary(s,spv::OpINotEqual,b,Extract(s,u,result,3),ConstantU32(s,0));
  const auto counter=AstroCapturePointer(s,Binary(s,spv::OpIAdd,u,ConstantU32(s,base),Select(s,u,used,ConstantU32(s,N::UsedRays),ConstantU32(s,N::FallbackRays))));
  const auto ignored=s.builder.AllocateId();
  s.builder.AddFunction(spv::OpAtomicIAdd,u,ignored,counter,ConstantU32(s,spv::ScopeDevice),ConstantU32(s,spv::MemorySemanticsMaskNone),ConstantU32(s,1));
  s.builder.AddFunction(spv::OpReturnValue,result);s.builder.AddFunction(spv::OpFunctionEnd);
}
uint32_t EmitAstroTrace(ValueEmitContext& ctx,const IR::Inst& inst) {
  auto& s=ctx.state;
  if(!s.astro_trace_function)ctx.Fail(inst,"native Astro helper missing");
  return EmitValueOrDefaultIfCondition(s,ctx.Arg(inst,4),TypeU32Vector(s,4),ConstantU32CompositeZero(s,4),[&] {
    const auto value=s.builder.AllocateId();
    s.builder.AddFunction(spv::OpFunctionCall,TypeU32Vector(s,4),value,s.astro_trace_function,
      ctx.Arg(inst,0),ctx.Arg(inst,1),ctx.Arg(inst,2),ctx.Arg(inst,3));return value;
  });
}
