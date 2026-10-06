// Bitcasts are bit-preserving typed SSA views, including across guarded memory blocks.
void TestTypedBitcastViews() {
  using namespace ShaderRecompiler::Spirv::Emitter;
  ShaderRecompiler::IR::Program program;
  program.stage = ShaderType::Compute;
  EmitterState state(program, ShaderStageInputInfo{});
  auto& builder = state.builder;
  builder.RequireCapability(spv::CapabilityShader);
  builder.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  const auto uint_type = TypeU32(state);
  const auto bool_type = TypeBool(state);
  const auto function = builder.AllocateId();
  const auto signature = builder.Type(spv::OpTypeFunction, uint_type, uint_type, bool_type);
  builder.AddFunction(spv::OpFunction, uint_type, function, spv::FunctionControlMaskNone, signature);
  const auto bits = builder.AllocateId();
  const auto active = builder.AllocateId();
  builder.AddFunction(spv::OpFunctionParameter, uint_type, bits);
  builder.AddFunction(spv::OpFunctionParameter, bool_type, active);
  EmitLabel(state, builder.AllocateId());
  const auto view = EmitBitcastU32ToF32(state, bits);
  Check(EmitBitcastF32ToU32(state, view) == bits, "inverse bitcast must retain the original bits");
  EmitValueOrZeroIfCondition(state, active, [&] {
    Check(EmitBitcastU32ToF32(state, bits) == view, "guard must retain dominating typed views");
    EmitValueOrZeroIfCondition(state, active, [&] {
      Check(EmitBitcastF32ToU32(state, view) == bits, "inverse view must work in nested guards");
      return bits;
    });
    return bits;
  });
  Check(EmitBitcastU32ToF32(state, bits) == view, "guard merge must retain dominating typed views");

  const auto other = Binary(state, spv::OpBitwiseXor, uint_type, bits, ConstantU32(state, 1));
  uint32_t conditional_view = 0;
  EmitValueOrZeroIfCondition(state, active, [&] {
    conditional_view = EmitBitcastU32ToF32(state, other);
    return other;
  });
  Check(EmitBitcastU32ToF32(state, other) != conditional_view,
        "a view defined only in the taken arm must not escape its guard");
  for (uint32_t pattern : {0x80000000u, 0x7f800001u, 0x7fc12345u, 0xffc12345u, 0x00000001u}) {
    const auto value = ConstantU32(state, pattern);
    Check(EmitBitcastF32ToU32(state, EmitBitcastU32ToF32(state, value)) == value,
          "typed views must retain signed zero, NaN payloads and denormal bits");
  }
  builder.AddFunction(spv::OpReturnValue, bits);
  builder.AddFunction(spv::OpFunctionEnd);

  const auto main = builder.AllocateId();
  builder.AddEntryPoint(spv::ExecutionModelGLCompute, main, "main", {});
  builder.AddExecutionMode(main, spv::ExecutionModeLocalSize, 1u, 1u, 1u);
  builder.AddFunction(spv::OpFunction, TypeVoid(state), main, spv::FunctionControlMaskNone,
                      TypeFunction(state));
  EmitLabel(state, builder.AllocateId());
  builder.AddFunction(spv::OpFunctionCall, uint_type, builder.AllocateId(), function,
                      ConstantU32(state, 0x7f800001u), ConstantBool(state, true));
  builder.AddFunction(spv::OpReturn);
  builder.AddFunction(spv::OpFunctionEnd);
  CheckSpirvBinaryValidates(builder.Build());
}
