// CPU translation/validation checks: no Vulkan device is created.
void TestLocalAddressStoreProof() {
  using namespace ShaderRecompiler::IR;
  for (uint32_t scenario = 0; scenario < 15; ++scenario) {
    Program program;
    program.wave_size = 32;
    program.block_storage.push_back(std::make_unique<Block>());
    auto* block = program.block_storage.back().get();
    program.blocks.push_back(block);
    program.memory_info.push_back({.kind = ResourceKind::Global});
    IREmitter ir(block);
    const auto zero = U32(Value(0u));
    const auto lane = U32(ir.Emit(ValueOpcode::LaneId));
    const auto predicate = ir.INotEqual(lane, zero);
    auto high = ir.Select(predicate, U32(Value(SharedApertureHigh)),
                          U32(Value(PrivateApertureHigh)));
    auto active = ir.GetExec();
    if (scenario == 1 || scenario == 9 || scenario == 13 || scenario == 14)
      high = lane; // Unknown addresses must retain the access.
    if (scenario == 2 || scenario == 3) {
      high = ir.Select(predicate, U32(Value(scenario == 2 ? SharedApertureHigh : 1u)), lane);
      active = ir.LogicalAnd(active, predicate);
    }
    if (scenario == 4) {
      const auto bit = ir.BitwiseAnd(lane, U32(Value(1u)));
      const auto choice = ir.ShiftLeftLogical(bit, U32(Value(31u)));
      high = U32(ir.Emit(ValueOpcode::UMedTri32,
          {Value(SharedApertureHigh), Value(PrivateApertureHigh), choice}));
    }
    if (scenario == 5 || scenario == 6) {
      auto& phi = block->AppendNewInst(ValueOpcode::Phi);
      phi.SetFlags(Type::U32);
      phi.AddPhiOperand(block, Value(SharedApertureHigh));
      phi.AddPhiOperand(block, Value(PrivateApertureHigh));
      high = U32(Value(&phi));
      if (scenario == 6) block->AddBranch(block); // Loop phis require a fixpoint proof.
    }
    const auto local = ir.LogicalOr(ir.IEqual(high, U32(Value(SharedApertureHigh))),
                                    ir.IEqual(high, U32(Value(PrivateApertureHigh))));
    const auto mask = scenario == 7 || scenario == 10 ? U1(Value(false)) : ir.LogicalAnd(active, ir.LogicalNot(local));
    if (scenario >= 8)
      ir.Emit(scenario == 11 || scenario == 13 ? ValueOpcode::LoadAddressU8 :
              scenario == 12 || scenario == 14 ? ValueOpcode::LoadAddressU16 :
                                               ValueOpcode::LoadAddressU32,
            {ir.Emit(ValueOpcode::GetAddressResource, {zero, high}), zero, high, mask},
            MemoryFlags{.index = 0});
    else ir.Emit(ValueOpcode::StoreAddressU32,
            {ir.Emit(ValueOpcode::GetAddressResource, {zero, high}), zero, high, zero, mask},
            MemoryFlags{.index = 0});
    const bool removable = scenario == 0 || scenario == 2 || scenario == 4 || scenario == 5 || scenario == 7 || scenario == 8 || scenario == 10 || scenario == 11 || scenario == 12;
    const auto removed = SimplifyLocalAddressStores(program);
    if (removed != removable) std::fprintf(stderr, "local proof scenario %u: removed=%u\n", scenario, removed);
    Check(removed == removable,
          "local store proof discarded a possible global write or missed a local aperture");
  }
}

void TestWolverineInstructions() {
  using namespace ShaderRecompiler;
  TestLocalAddressStoreProof();
  const auto count = [](const IR::Program& program, IR::ValueOpcode opcode) {
    size_t result = 0;
    for (const auto* block : program.blocks)
      result += std::ranges::count(*block, opcode, &IR::Inst::GetOpcode);
    return result;
  };
  const auto compile = [](std::span<const uint32_t> code, uint32_t wave) {
    auto compute = RegressionComputeInputInfo();
    compute.wave_size = wave;
    compute.threads_num[0] = wave;
    compute.scratch_size_dwords = 64;
    auto options = MakeCompileOptions(ShaderType::Compute);
    options.input_info.compute = &compute;
    options.shader_hash = 0x123456789abcdef0;
    auto result = RecompileForTest(code, options);
    CheckSpirvBinaryValidates(result.spirv);
    return result;
  };
  const auto mov = [](std::vector<uint32_t>& code, uint32_t reg, uint32_t value) {
    code.insert(code.end(), {EncodeVop1(0x01, reg, 255), value});
  };
  for (uint32_t trap : {1u, 0x7fu}) {
    const std::array code{EncodeSopp(0x12, trap), EncodeSopp(0x01)};
    auto result = compile(code, 32);
    Check(count(result.program, IR::ValueOpcode::ShaderTrap) == (trap == 0x7fu),
          "reserved trap removed or ordinary guest trap became fatal");
    Check(SpirvInstructionOpcodeCount(result.spirv, spv::OpAtomicCompareExchange) ==
              (trap == 0x7fu), "shader trap did not elect one payload writer");
    Check(result.program.info.uses_dma == (trap == 0x7fu), "trap missing DMA binding");
    if (trap == 0x7fu) {
      Check(IR::FindBinding(result.program.bindings, IR::DescriptorBindingKind::FaultBuffer),
            "reserved trap has no fault buffer");
      std::vector<uint8_t> encoded, reencoded;
      const auto metadata = std::move(result.program).TakeCompiledInfo();
      IR::EncodeCompiledShaderInfo(metadata, encoded);
      IR::CompiledShaderInfo restored;
      Check(IR::DecodeCompiledShaderInfo(encoded, restored), "trap metadata codec failed");
      IR::EncodeCompiledShaderInfo(restored, reencoded);
      Check(encoded == reencoded && restored.info.uses_dma, "trap metadata codec lost bindings");
    }
  }
  for (uint32_t wave : {32u, 64u}) for (uint32_t base : {235u, 237u}) {
    for (const auto widths : {std::pair{0x08u, 0x18u}, std::pair{0x0au, 0x1au},
                              std::pair{0x0cu, 0x1cu}}) {
      std::vector<uint32_t> code{EncodeSop1(0x04, 8, base),
                                EncodeVop1(0x01, 20, 8), EncodeVop1(0x01, 21, 9)};
      mov(code, 30, 42);
      code.insert(code.end(), {EncodeFlat0(widths.second, 0), EncodeFlat1(0, 0x7d, 30, 20),
                               EncodeFlat0(widths.first, 0), EncodeFlat1(24, 0x7d, 0, 20),
                               EncodeDs0(0x0d, 32), EncodeDs1(0, 24, 20), EncodeSopp(0x01)});
      auto result = compile(code, wave);
      Check(!result.program.has_address_writes && !result.program.info.uses_dma,
            "local FLAT access retained global ownership/DMA dependency");
      Check(DisassembleSpirvBinary(result.spirv).find("get_bda_pointer") == std::string::npos,
            "local FLAT access reached BDA mapper");
    }
  }
  for (const auto operand : {235u, 236u, 237u, 238u}) {
    const std::array code{EncodeSop1(0x04, 8, operand), EncodeSopp(0x01)};
    Decoder::Program decoded;
    Decoder::DecodeProgram(code, decoded);
    Check(decoded.instructions.front().opcode == Decoder::Opcode::S_MOV_B64,
          "local aperture register did not decode");
    auto compute = RegressionComputeInputInfo();
    Frontend::TranslateOptions options{.stage = ShaderType::Compute};
    options.input_info.compute = &compute;
    auto program = Frontend::TranslateProgram(decoded, CFG::BuildGraph(decoded), options);
    bool found = false;
    for (const auto* block : program.blocks) for (const auto& inst : *block) {
      if (inst.GetOpcode() == IR::ValueOpcode::SetScalarRegister &&
          inst.Arg(0).ScalarRegister() == static_cast<IR::ScalarReg>(9)) {
        const auto high = inst.Arg(1).Resolve();
        found = high.IsImmediate() && high.U32() ==
            (operand < 237u ? Decoder::SharedApertureHigh : Decoder::PrivateApertureHigh);
      }
    }
    Check(found, "64-bit aperture source lost high tag");
  }
  for (const auto address : {std::pair{0u, 0x78000000u},
                              std::pair{0xfffffffcu, Decoder::SharedApertureHigh},
                              std::pair{0u, 0u}}) {
    std::vector<uint32_t> code;
    mov(code, 20, address.first); mov(code, 21, address.second);
    if (address.second != 0)
      code.insert(code.end(), {EncodeFlat0(0x1c, 0, 4), EncodeFlat1(0, 0x7d, 30, 20)});
    code.insert(code.end(), {EncodeFlat0(0x0c, 0, 4), EncodeFlat1(24, 0x7d, 0, 20),
                             EncodeDs0(0x0d), EncodeDs1(0, 24, 30), EncodeSopp(0x01)});
    const auto result = compile(code, 32);
    const auto text = DisassembleSpirvBinary(result.spirv);
    if (address.second == 0) {
      Check(text.find("1099511627776") != std::string::npos && text.find("OpULessThan") != std::string::npos,
            "global FLAT access lost 40-bit page-table guard");
    } else {
      Check(!result.program.has_address_writes && !result.program.info.uses_dma,
            "noncanonical/offset-carry FLAT access escaped its aperture guard");
    }
  }
  for (uint32_t wave : {32u, 64u}) for (uint32_t nsa : {0u, 3u}) {
    for (const auto location : {std::pair{0x100u, 0u}, std::pair{0x100u, 4u},
                                std::pair{0x100u, 5u}, std::pair{0x100u, (255u << 3u) | 5u},
                                std::pair{0xffffff00u, (1022u << 3u) | 5u},
                                std::pair{0xffffff00u, (1023u << 3u) | 4u},
                                std::pair{0xffffff00u, (1023u << 3u) | 5u},
                                std::pair{0xffffff00u, (1024u << 3u) | 5u},
                                std::pair{0u, 5u}}) {
      std::vector<uint32_t> code;
      // BVH descriptors encode base >> 8. 0xffffff00 therefore addresses the
      // last 64 KiB of the 40-bit aperture: node 1023 fits a half node, but its
      // full node crosses the page-table limit. High word 1 encodes 1 << 40.
      const std::array descriptor{location.first, location.first == 0 ? 1u : 0u,
                                  location.first == 0xffffff00u ? 1024u : 256u, 0x81000000u};
      for (uint32_t word = 0; word < 4; ++word)
        code.insert(code.end(), {EncodeSMovB32(word, 255), descriptor[word]});
      for (uint32_t word = 0; word < 11; ++word)
        mov(code, 40 + word, word == 0 ? location.second : word == 1 ? 0x40c00000u :
            word == 7 ? 0x3f800000u : word >= 8 ? 0x7f800000u : 0u);
      code.insert(code.end(), {EncodeMimg0(0xe6, 0xf) | (1u << 15u) | (nsa << 1u),
                               EncodeMimg1(20, 0, 0, 40)});
      if (nsa) code.insert(code.end(), {0x2c2b2a29u, 0x302f2e2du, 0x00003231u});
      code.insert(code.end(), {EncodeDs0(0xdf), EncodeDs1(0, 20, 30), EncodeSopp(0x01)});
      Decoder::Program decoded;
      Decoder::DecodeProgram(code, decoded);
      Check(decoded.instructions.back().opcode == Decoder::Opcode::S_ENDPGM,
            "BVH decoder stopped before shader end");
      const auto result = compile(code, wave);
      Check(count(result.program, IR::ValueOpcode::BvhIntersect) == 1, "BVH did not reach emission");
      const auto text = DisassembleSpirvBinary(result.spirv);
      Check(text.find("bvh_intersect") != std::string::npos &&
                text.find("1099511627776") != std::string::npos &&
                text.find("NoContraction") != std::string::npos,
            "BVH lost checked mapping or watertight arithmetic");
      Check(SpirvInstructionOpcodeCount(result.spirv, spv::OpFunctionCall) >= 3,
            "FP32 BVH did not independently translate both page blocks");
    }
  }
  for (uint32_t modifiers : {0u, 2u, 4u}) {
    const std::array code{EncodeMimg0(0xe6, 0xf) | modifiers, EncodeMimg1(20, 0, 0, 40), 0u, 0u};
    Decoder::Instruction instruction;
    Decoder::DecodeInstruction(code, 0, instruction);
    Check(instruction.opcode == Decoder::Opcode::UNSUPPORTED, "malformed BVH accepted");
  }
  const std::array bvh64{EncodeMimg0(0xe7, 0xf) | (1u << 15u), EncodeMimg1(20, 0, 0, 40)};
  Decoder::Instruction unsupported;
  Decoder::DecodeInstruction(bvh64, 0, unsupported);
  Check(unsupported.opcode == Decoder::Opcode::UNSUPPORTED, "unimplemented BVH64 silently accepted");
  const std::array ds64{EncodeDs0(0x40), EncodeDs1(0, 10, 20), EncodeSopp(0x01)};
  const auto atomic = compile(ds64, 32);
  const auto atomic_text = DisassembleSpirvBinary(atomic.spirv);
  Check(count(atomic.program, IR::ValueOpcode::SharedAtomicIAdd64) == 1 &&
            SpirvInstructionOpcodeCount(atomic.spirv, spv::OpAtomicIAdd) == 1 &&
            atomic_text.find("Int64Atomics") != std::string::npos,
        "DS_ADD_U64 lost its genuine 64-bit shared atomic path");
}

// BVH forms the recompiler does not lower (BVH64, malformed BVH32) skip the dispatch or draw in
// every stage instead of ending the emulator in the CFG builder. Ordinary shaders, and BVH-shaped
// literals and operand words, are not skipped.
void TestUnsupportedBvhSkipsDispatch() {
  namespace D = ShaderRecompiler::Decoder;
  const auto unsupported_bvh = [](std::span<const uint32_t> code) {
    D::Program decoded;
    D::DecodeProgram(code, decoded);
    return std::ranges::any_of(decoded.instructions, [](const D::Instruction &inst) {
      return inst.opcode == D::Opcode::UNSUPPORTED && inst.family == D::Family::MIMG &&
             (inst.opcode_id == 0xe6u || inst.opcode_id == 0xe7u);
    });
  };
  const auto skips = [&](std::initializer_list<uint32_t> list, const char *message) {
    const std::span<const uint32_t> code{list.begin(), list.size()};
    Check(unsupported_bvh(code), message);
    // Graphics stages too: a BVH pixel shader used to abort in the CFG builder.
    for (const auto stage : {ShaderType::Compute, ShaderType::Vertex, ShaderType::Pixel}) {
      const auto result = ShaderRecompiler::TranslateProgram(code, MakeCompileOptions(stage));
      Check(result.skip_dispatch && result.program.blocks.empty() && result.cfg_dump.empty(),
            message);
    }
  };
  skips({0xf19c9f01u, 0x00081014u, EncodeSopp(0x01)}, "RDNA2 BVH64 instruction did not skip");
  skips({EncodeMimg0(0xe6, 0xf), EncodeMimg1(20, 0, 0, 40), EncodeSopp(0x01)},
        "BVH32 without R128 did not skip");

  const auto plain = [&](std::initializer_list<uint32_t> list, const char *message) {
    Check(!unsupported_bvh({list.begin(), list.size()}), message);
  };
  plain({EncodeSMovB32(0, 255), 0xf1989f01u, EncodeSopp(0x01)},
        "scalar literal was mistaken for a BVH instruction");
  plain({EncodeMimg0(0x20, 0xf) | 6u, EncodeMimg1(0, 0, 0, 0), 0xf1989f01u, 0xf19c9f01u, 0u,
         EncodeSopp(0x01)},
        "NSA address words were mistaken for BVH instructions");
  plain({EncodeMimg0(0x20, 0xf), EncodeMimg1(0, 0, 0, 0), EncodeSopp(0x01)},
        "ordinary image shader was taken for a BVH instruction");
  const std::array ordinary_code{EncodeSMovB32(0, 255), 0xf1989f01u, EncodeSopp(0x01)};
  for (const auto stage : {ShaderType::Compute, ShaderType::Vertex, ShaderType::Pixel}) {
    const auto options = MakeCompileOptions(stage);
    Check(!ShaderRecompiler::TranslateProgram(ordinary_code, options).skip_dispatch,
          "BVH-shaped literal marked an ordinary shader for skipping");
    const auto compiled = RecompileForTest(ordinary_code, options);
    Check(!compiled.spirv.empty(), "ordinary shader did not compile");
    CheckSpirvBinaryValidates(compiled.spirv);
  }
}

void TestRayTracingDispatchDetection() {
  TestWolverineInstructions();
  TestUnsupportedBvhSkipsDispatch();
}

// The traversal-loop shape of Astro Bot's BVH shaders (380bb9d636390bae and 26 more failed spirv-val
// with "Selection must be structured"), also in Astro's Playroom's lit passes (CS 5df28d2058caf728,
// where RADV's spirv_to_nir rejected the module and crashed): the loop body's first conditional
// either stays in the loop or enters a region with its own loop that leaves through the loop's
// merge, and the body has another exit. The conditional must get a selection merge (a synthetic
// block on the region's exit edge), not be treated as loop control.
//   s0 = 0
//   outer: if (s0 < 4) goto body
//          s1 = 0; inner: s1 += 1; if (s1 < 3) goto inner; goto done   <- the break region
//   body:  s0 += 1; if (s0 == 50) goto done; goto outer                <- the other exit
//   done:  ...
std::vector<uint32_t> TraversalLoopBreakRegionCode() {
  return {
      EncodeSMovB32(0, 128),            // 0: s_mov_b32 s0, 0
      EncodeSopc(0x0a, 0, 132),         // 1: outer: s_cmp_lt_u32 s0, 4
      EncodeSopp(0x05, 5),              // 2: s_cbranch_scc1 body
      EncodeSMovB32(1, 128),            // 3: s_mov_b32 s1, 0
      EncodeSop2(0x00, 1, 1, 129),      // 4: inner: s_add_u32 s1, s1, 1
      EncodeSopc(0x0a, 1, 131),         // 5: s_cmp_lt_u32 s1, 3
      EncodeSopp(0x05, 0xfffdu),        // 6: s_cbranch_scc1 inner
      EncodeSopp(0x02, 4),              // 7: s_branch done
      EncodeSop2(0x00, 0, 0, 129),      // 8: body: s_add_u32 s0, s0, 1
      EncodeSopc(0x06, 0, 178),         // 9: s_cmp_eq_u32 s0, 50
      EncodeSopp(0x05, 1),              // 10: s_cbranch_scc1 done
      EncodeSopp(0x02, 0xfff5u),        // 11: s_branch outer
      EncodeSopp(0x01),                 // 12: done: s_endpgm
  };
}

void TestTraversalLoopBreakRegion() {
  namespace D = ShaderRecompiler::Decoder;
  namespace CFG = ShaderRecompiler::CFG;
  const auto code = TraversalLoopBreakRegionCode();
  D::Program decoded;
  D::DecodeProgram(code, decoded);
  auto graph = CFG::BuildGraph(decoded);
  Check(!graph.unsupported && !graph.irreducible && CFG::Structurize(graph),
        "traversal-loop break region did not structurize");
  // The conditional at pc 4 (the loop's first body block) has a merge: a synthetic block that
  // breaks to the loop merge.
  const auto conditional = std::ranges::find_if(graph.blocks, [](const CFG::BasicBlock &block) {
    return block.start_pc == 4u && block.inst_begin != block.inst_end &&
           block.terminator.kind == CFG::TerminatorKind::ConditionalBranch;
  });
  Check(conditional != graph.blocks.end(), "traversal-loop conditional not found");
  const auto *merge = graph.FindBlock(conditional->terminator.merge_block);
  Check(merge != nullptr && merge->inst_begin == merge->inst_end &&
            merge->terminator.kind == CFG::TerminatorKind::Branch &&
            graph.FindBlock(merge->terminator.true_block) != nullptr &&
            graph.FindBlock(merge->terminator.true_block)->start_pc == 48u,
        "traversal-loop conditional lacks its synthetic merge before the loop exit");
  for (const auto stage : {ShaderType::Compute, ShaderType::Pixel}) {
    const auto compiled = RecompileForTest(code, MakeCompileOptions(stage));
    Check(!compiled.program.dispatcher_fallback,
          "traversal-loop shader fell back to the dispatcher");
    CheckSpirvBinaryValidates(compiled.spirv);
  }
}
