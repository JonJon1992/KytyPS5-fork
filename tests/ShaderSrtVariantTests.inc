// KYTY_SRT_VARIANT_READS GPU tests (included by ShaderRecompilerComputeTests.cpp after
// ShaderCodegenTests.inc).
//
// Astro Bot's tiled deferred-lighting kernels walk a BVH: they load an instance record through a
// loop-carried pointer (S_LOAD) and read the record's V# with S_BUFFER_LOAD. Without the switch
// such a record read is a flat SRT slot that cannot be evaluated before the dispatch (the pipeline
// cache skips the source after its first failure), and an S_BUFFER_LOAD through a runtime V# fails
// resource tracking. With it, both
// are read in the shader through BDA, with the RDNA2 bounds of a bound scalar buffer.

namespace SrtVariantTests {

using CodegenTests::ScopedCodegenOptions;
using ShaderRecompiler::GetCodegenOptions;

// S_BUFFER_LOAD_DWORDX4 through V#s the shader selects at runtime. RDNA2 ISA 7.2.1: only base,
// stride and num_records are used, addr = (base + OFFSET + SOFFSET) & ~3. The bound matches a bound
// scalar buffer (stride 0: num_records bytes, else stride * num_records), and a dword reads 0
// unless all four of its bytes are below it.
TestCase ScalarBufferLoadsGpuSelectedDescriptors() {
  using O = ShaderOpcode;
  constexpr uint64_t GuestBase = 0x0000000118000000ull;
  struct DescriptorCase {
    u32 stride, records, offset, soffset;
  };
  const DescriptorCase cases[] = {
      {0, 16, 8, 0},  // dwords 2-3 in, 4-5 past the 16-byte bound
      {0, 24, 8, 0},  // all four in
      {12, 2, 12, 0}, // stride * records = 24 bytes: dwords 3-5 in, 6 out
      {0, 0, 0, 0},   // empty
      {0, 20, 4, 8},  // SOFFSET is added: dwords 3-4 in, 5-6 out
      {0, 32, 10, 0}, // the two LSBs of the offset are ignored: reads from byte 8
      {4, 5, 8, 0},   // 20 bytes: dwords 2-4 in, 5 out
      {0, 15, 8, 0},  // 15 bytes: dword 2 in, dword 3 (bytes 12-15) out
  };
  constexpr u32 Count = static_cast<u32>(std::size(cases));
  TestCase test;
  test.name = "ScalarBufferLoadsGpuSelectedDescriptors";
  test.initial.resize(2048);
  for (u32 i = 0; i < Count; ++i) {
    const u32 data_offset = 4096 + i * 128;
    const uint64_t base = GuestBase + data_offset;
    const std::array<u32, 4> descriptor{static_cast<u32>(base),
                                        (cases[i].stride << 16u) | static_cast<u32>(base >> 32u),
                                        cases[i].records, 0x5204u};
    std::copy(descriptor.begin(), descriptor.end(), test.initial.begin() + 130 + i * 30);
    for (u32 word = 0; word < 32; ++word) {
      test.initial[data_offset / 4 + word] = i * 100 + word + 1;
    }
  }
  for (u32 i = 0; i < Count; ++i) {
    // Select the table entry through a GPU load and readfirstlane, not host constants.
    const u32 selected = Count - 1 - i;
    const auto &input = cases[selected];
    test.initial[64 + i] = selected;
    AppendVMovU32(&test.code, 30, (64 + i) * 4);
    AppendBufferLoadDword(&test.code, 0, 30);
    test.code.push_back(EncodeVop1(0x02, 20, Vgpr(0))); // v_readfirstlane_b32 s20, v0
    test.code.push_back(EncodeSop2(0x26, 20, 20, 255)); // s_mul_i32 s20, s20, 120
    test.code.push_back(120);
    test.code.push_back(EncodeSmem0(0x0a, 8, 0)); // s_buffer_load_dwordx4 s[8:11], s[0:3]
    test.code.push_back(EncodeSmem1(520, 20));
    AppendSMovLiteral(&test.code, 22, input.soffset);
    test.code.push_back(EncodeSmem0(0x0a, 12, 4)); // s_buffer_load_dwordx4 s[12:15], s[8:11]
    test.code.push_back(EncodeSmem1(input.offset, 22));
    const u32 size = input.stride == 0 ? input.records : input.stride * input.records;
    const u32 first = (input.offset + input.soffset) & ~3u;
    for (u32 component = 0; component < 4; ++component) {
      AppendStoreSgpr(&test.code, 12 + component, i * 4 + component);
      const u32 byte = first + component * 4;
      test.expected.push_back(byte + 4 <= size ? selected * 100 + byte / 4 + 1 : 0);
    }
  }
  AppendEnd(&test.code);
  test.bda_mappings = {{GuestBase, 0}};
  test.opcodes = {O::V_MOV_B32, O::S_MOV_B32, O::BUFFER_LOAD_DWORD, O::V_READFIRSTLANE_B32,
                  O::S_MUL_I32, O::S_BUFFER_LOAD_DWORDX4, O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  test.required_spirv = {"OpConvertUToPtr", "PhysicalStorageBuffer"};
  return test;
}

// The BVH traversal shape: a loop follows `next` pointers through records (s_load_dwordx2 through
// the loop-carried pointer) and reads each record's V# (s_load_dwordx4 at +16) to
// s_buffer_load_dwordx4 its data. Record 0: 16-byte buffer, record 1: stride 4 x 6 records,
// record 2: empty; the walk ends at a null `next`.
TestCase ScalarLoadsThroughLoopCarriedRecords() {
  using O = ShaderOpcode;
  constexpr uint64_t GuestBase = 0x0000000120000000ull;
  constexpr u32 Records = 3;
  constexpr u32 RecordBytes = 4096;
  constexpr u32 DataBytes = 6144;
  struct Buffer {
    u32 stride, records;
  };
  const Buffer buffers[Records] = {{0, 16}, {4, 6}, {0, 0}};
  TestCase test;
  test.name = "ScalarLoadsThroughLoopCarriedRecords";
  test.initial.resize(2048);
  for (u32 n = 0; n < Records; ++n) {
    const u32 record = (RecordBytes + n * 64) / 4;
    const uint64_t next = n + 1 < Records ? GuestBase + RecordBytes + (n + 1) * 64 : 0;
    const uint64_t data = GuestBase + DataBytes + n * 256;
    test.initial[record + 0] = static_cast<u32>(next);
    test.initial[record + 1] = static_cast<u32>(next >> 32u);
    test.initial[record + 4] = static_cast<u32>(data);
    test.initial[record + 5] = (buffers[n].stride << 16u) | static_cast<u32>(data >> 32u);
    test.initial[record + 6] = buffers[n].records;
    test.initial[record + 7] = 0x5204u;
    for (u32 word = 0; word < 64; ++word) {
      test.initial[(DataBytes + n * 256) / 4 + word] = 1000 + n * 100 + word;
    }
  }
  auto &code = test.code;
  const uint64_t root = GuestBase + RecordBytes;
  AppendSMovLiteral(&code, 20, static_cast<u32>(root));
  AppendSMovLiteral(&code, 21, static_cast<u32>(root >> 32u));
  AppendSMovLiteral(&code, 30, 0); // output byte offset
  const auto loop = code.size();
  code.push_back(EncodeSopc(0x13, 20, InlineU32(0))); // s_cmp_lg_u64 s[20:21], 0
  const auto exit_branch = code.size();
  code.push_back(EncodeSopp(0x04, 0)); // s_cbranch_scc0 done (patched below)
  code.push_back(EncodeSmem0(0x02, 24, 10)); // s_load_dwordx4 s[24:27], s[20:21], 16
  code.push_back(EncodeSmem1(16, 125));
  code.push_back(EncodeSmem0(0x01, 22, 10)); // s_load_dwordx2 s[22:23], s[20:21], 0
  code.push_back(EncodeSmem1(0, 125));
  code.push_back(EncodeSopp(0x0c, 0xc07f)); // s_waitcnt lgkmcnt(0)
  code.push_back(EncodeSmem0(0x0a, 36, 12)); // s_buffer_load_dwordx4 s[36:39], s[24:27], 8
  code.push_back(EncodeSmem1(8, 125));
  code.push_back(EncodeSopp(0x0c, 0xc07f));
  code.push_back(EncodeVop1(0x01, 31, 30)); // v_mov_b32 v31, s30
  for (u32 component = 0; component < 4; ++component) {
    code.push_back(EncodeVop1(0x01, 1, 36 + component)); // v_mov_b32 v1, s[36 + component]
    code.push_back(EncodeVop2(0x25, 32, InlineU32(component * 4), 31)); // v32 = v31 + 4k
    AppendBufferStoreDword(&code, 1, 32);
  }
  code.push_back(EncodeSop2(0x00, 30, 30, InlineU32(16))); // s_add_u32 s30, s30, 16
  code.push_back(EncodeSop1(0x04, 20, 22)); // s_mov_b64 s[20:21], s[22:23]
  const auto back_branch = code.size();
  code.push_back(EncodeSopp(0x02, static_cast<u32>(static_cast<int32_t>(loop) -
                                                   static_cast<int32_t>(back_branch + 1)) &
                                      0xffffu)); // s_branch loop
  code[exit_branch] = EncodeSopp(0x04, static_cast<u32>(code.size() - (exit_branch + 1)));
  AppendEnd(&code);
  for (u32 n = 0; n < Records; ++n) {
    const u32 size = buffers[n].stride == 0 ? buffers[n].records : buffers[n].stride * buffers[n].records;
    for (u32 component = 0; component < 4; ++component) {
      const u32 byte = 8 + component * 4;
      test.expected.push_back(byte + 4 <= size ? 1000 + n * 100 + byte / 4 : 0);
    }
  }
  test.bda_mappings = {{GuestBase, 0}};
  test.opcodes = {O::S_MOV_B32, O::S_CMP_LG_U64, O::S_CBRANCH_SCC0, O::S_LOAD_DWORDX4,
                  O::S_LOAD_DWORDX2, O::S_WAITCNT, O::S_BUFFER_LOAD_DWORDX4, O::V_MOV_B32,
                  O::V_ADD_NC_U32, O::BUFFER_STORE_DWORD, O::S_ADD_U32, O::S_MOV_B64,
                  O::S_BRANCH, O::S_ENDPGM};
  test.required_spirv = {"OpConvertUToPtr", "PhysicalStorageBuffer"};
  return test;
}

namespace UnmappedFlatRead {
constexpr uint64_t Unmapped = 0x0000010000000000ull;
uint32_t refused = 0;

bool GuestMapped(uint64_t address, uint64_t size) {
  if (address >= Unmapped && address < Unmapped + 0x1000u && size <= 0x1000u) {
    ++refused;
    return false;
  }
  return true;
}

bool Probe(void *, uint64_t address, std::span<uint32_t> values) {
  if (address < 0x10000u ||
      (address >= Unmapped && address < Unmapped + 0x1000u)) {
    return false;
  }
  std::memcpy(values.data(), reinterpret_cast<const void *>(address), values.size_bytes());
  return true;
}
} // namespace UnmappedFlatRead

// An optional null or unmapped pointer must not be dereferenced during SRT materialization.
void NullBasedFlatReadReadsZero() {
  namespace IR = ShaderRecompiler::IR;
  std::vector<u32> code {
      EncodeSmem0(0x01, 8, 0), EncodeSmem1(0, 125),
      EncodeSopp(0x0c, 0xc07f),
      EncodeSmem0(0x01, 10, 4), EncodeSmem1(0x30, 125),
      EncodeSopp(0x0c, 0xc07f),
  };
  const u32 header_pc = 3u * sizeof(u32);
  AppendStoreSgpr(&code, 10, 0);
  AppendStoreSgpr(&code, 11, 1);
  AppendEnd(&code);
  static std::array<uint64_t, 16> table{};
  static std::array<u32, 32> header{};
  header[12] = 0x1234u;
  header[13] = 0x5678u;

  const auto run = [&](uint64_t pointer, IR::SrtMappedRange mapped, IR::SrtMemoryReader probe) {
    table[0] = pointer;
    auto user_data = MakeNativeUserData(nullptr);
    const uint64_t table_address = reinterpret_cast<uint64_t>(table.data());
    user_data[0] = static_cast<u32>(table_address);
    user_data[1] = static_cast<u32>(table_address >> 32u);
    ShaderComputeInputInfo info{};
    ShaderRecompiler::CompileOptions options{};
    options.stage = ShaderType::Compute;
    options.input_info.compute = &info;
    options.user_data = user_data;
    const auto translated = ShaderRecompiler::TranslateProgram(code, options);
    Require("NullBasedFlatReadReadsZero", "translate", !translated.skip_dispatch,
            "pointer-chasing shader did not translate");
    const auto plan = IR::ExtractResourcePlan(translated.program);
    IR::ResourceSnapshot resources;
    IR::ResourceSpecialization specialization;
    const IR::SrtRuntime runtime {
        .user_data = user_data,
        .shader_base = reinterpret_cast<uint64_t>(code.data()),
        .try_read_clean_backing = probe,
        .is_guest_mapped = mapped,
    };
    Require("NullBasedFlatReadReadsZero", "materialize",
            IR::MaterializeResources(plan, runtime, resources, specialization),
            "an unmapped optional pointer stopped materialization");
    std::vector<u32> values;
    for (const auto &read : plan.srt_reads) {
      const auto *inst = read.value.Resolve().TryInstruction();
      if (inst != nullptr && inst->Flags<IR::MemoryFlags>().pc == header_pc &&
          read.flat_offset < resources.flattened_srt.size()) {
        values.push_back(resources.flattened_srt[read.flat_offset]);
      }
    }
    std::ranges::sort(values);
    Require("NullBasedFlatReadReadsZero", "flat slots", values.size() == 2u,
            "header load did not produce two flat slots");
    return std::pair {values[0], values[1]};
  };

  for (const auto probe : {IR::SrtMemoryReader{}, IR::SrtMemoryReader{UnmappedFlatRead::Probe}}) {
    const auto null_values = run(0, nullptr, probe);
    Require("NullBasedFlatReadReadsZero", "null", null_values == std::pair{0u, 0u},
            "null header pointer was not read as zero");
    const auto valid_values = run(reinterpret_cast<uint64_t>(header.data()),
                                  UnmappedFlatRead::GuestMapped, probe);
    Require("NullBasedFlatReadReadsZero", "mapped",
            valid_values == std::pair{0x1234u, 0x5678u}, "mapped header was changed");
    UnmappedFlatRead::refused = 0;
    const auto absent_values = run(UnmappedFlatRead::Unmapped,
                                    UnmappedFlatRead::GuestMapped, probe);
    Require("NullBasedFlatReadReadsZero", "unmapped",
            absent_values == std::pair{0u, 0u} && UnmappedFlatRead::refused >= 2u,
            "unmapped header pointer was not guarded");
  }
  std::printf("[compute] NullBasedFlatReadReadsZero       ok\n");
}

void VariantPlanIsFound() {
  constexpr const char *name = "VariantPlanIsFound";
  auto codegen = GetCodegenOptions();
  codegen.srt_variant_reads = false;
  ScopedCodegenOptions scope(codegen);
  ShaderComputeInputInfo compute_info{};
  const auto translate = [&](const TestCase &test, std::array<u32, 64> &user_data) {
    user_data = MakeNativeUserData(nullptr);
    user_data[2] = static_cast<u32>(test.initial.size() * sizeof(u32));
    ShaderRecompiler::CompileOptions options;
    options.stage = ShaderType::Compute;
    options.input_info.compute = &compute_info;
    options.user_data = user_data;
    auto translated = ShaderRecompiler::TranslateProgram(test.code, options);
    Require(name, "translate", !translated.skip_dispatch && !translated.program.blocks.empty(),
            std::string(test.name) + " did not translate");
    return ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
  };

  const auto records = ScalarLoadsThroughLoopCarriedRecords();
  std::array<u32, 64> user_data{};
  const auto plan = translate(records, user_data);
  std::vector<uint32_t> loop_reads;
  {
    ShaderRecompiler::Decoder::Program decoded;
    ShaderRecompiler::Decoder::DecodeProgram(records.code, decoded);
    for (const auto &inst : decoded.instructions) {
      if (inst.opcode == ShaderOpcode::S_LOAD_DWORDX2 ||
          inst.opcode == ShaderOpcode::S_LOAD_DWORDX4 ||
          inst.opcode == ShaderOpcode::S_BUFFER_LOAD_DWORDX4) {
        loop_reads.push_back(inst.pc);
      }
    }
  }
  uint32_t pc = UINT32_MAX;
  Require(name, "loop-carried read",
          ShaderRecompiler::IR::FindVariantFlatRead(plan, pc) &&
              std::ranges::find(loop_reads, pc) != loop_reads.end(),
          "no flat read through the loop-carried pointer was found (pc " + Hex(pc) + ")");
  ShaderRecompiler::IR::ResourceSnapshot resources;
  ShaderRecompiler::IR::ResourceSpecialization specialization;
  const ShaderRecompiler::IR::SrtRuntime runtime{
      .user_data = user_data,
      .shader_base = reinterpret_cast<uint64_t>(records.code.data()),
      .read_memory = ReadTestMemory,
      .userdata = const_cast<std::vector<u32> *>(&records.initial),
  };
  Require(name, "no materialization",
          !ShaderRecompiler::IR::MaterializeResources(plan, runtime, resources, specialization),
          "a plan with a loop-carried flat read materialized");

  // A single pointer chase outside a loop can still be evaluated before dispatch.
  TestCase chase;
  chase.name = "PointerChase";
  chase.initial.resize(64);
  chase.code.push_back(EncodeSmem0(0x01, 8, 0));
  chase.code.push_back(EncodeSmem1(0, 125));
  chase.code.push_back(EncodeSopp(0x0c, 0xc07f));
  chase.code.push_back(EncodeSmem0(0x01, 10, 4));
  chase.code.push_back(EncodeSmem1(0x30, 125));
  chase.code.push_back(EncodeSopp(0x0c, 0xc07f));
  AppendStoreSgpr(&chase.code, 10, 0);
  AppendEnd(&chase.code);
  std::array<u32, 64> chase_user_data{};
  const auto chase_plan = translate(chase, chase_user_data);
  Require(name, "pointer chase", !ShaderRecompiler::IR::FindVariantFlatRead(chase_plan, pc),
          "a pointer loaded outside any loop was reported as loop-carried");
  std::printf("[compute] %-32s ok\n", name);
}

// The callees a skipped S_SWAPPC_B64 program jumps to, from the dispatch's user data: Psr's
// shader-mesh builders copy a callback record from user SGPRs into s[14:17] before each
// s_swappc_b64 s[14:15], s[14:15] (the swappc writes the return address into its destination pair,
// which does not make a target unknown). A call directly through user SGPRs names them. A target
// the program computes some other way is only counted as unresolved in the log line.
void CallTargetsFromUserData() {
  constexpr const char *name = "CallTargetsFromUserData";
  std::vector<u32> code;
  for (u32 i = 0; i < 4; ++i) {
    code.push_back(EncodeSMovB32(14 + i, 4 + i)); // s_mov_b32 s[14 + i], s[4 + i]
  }
  code.push_back(0xbe8e210eu); // s_swappc_b64 s[14:15], s[14:15]
  code.push_back(EncodeSMovB32(14, 8));
  code.push_back(EncodeSMovB32(15, 9));
  code.push_back(0xbe8e210eu); // s_swappc_b64 s[14:15], s[14:15]
  code.push_back(0xbe94210au); // s_swappc_b64 s[20:21], s[10:11]
  AppendEnd(&code);
  std::array<u32, 16> user_data{};
  user_data[4] = 0x00001000u;
  user_data[5] = 0x00000002u;
  user_data[8] = 0x00003000u;
  user_data[9] = 0x00000004u;
  user_data[10] = 0x00005000u;
  user_data[11] = 0x00010006u; // bits above 48 are not part of the address
  ShaderComputeInputInfo compute_info{};
  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Compute;
  options.input_info.compute = &compute_info;
  options.user_data = user_data;
  const auto result = ShaderRecompiler::TranslateProgram(code, options);
  Require(name, "skipped", result.skip_dispatch, "the calling program was not skipped");
  const auto &targets = result.call_targets;
  const auto has = [&](uint64_t address, u32 user_sgpr, u32 return_sgpr) {
    return std::ranges::any_of(targets, [&](const ShaderRecompiler::CallTarget &target) {
      return target.address == address && target.user_sgpr == user_sgpr &&
             target.return_sgpr == return_sgpr;
    });
  };
  Require(name, "targets",
          targets.size() == 3u && has(0x0000000200001000ull, 4, 14) &&
              has(0x0000000400003000ull, 8, 14) && has(0x0000000600005000ull, 10, 20),
          "the callees were not read from user data (" + std::to_string(targets.size()) +
              " found)");

  // A target the program computes from a user-data copy has no known callee.
  std::vector<u32> computed;
  computed.push_back(EncodeSMovB32(14, 4));
  computed.push_back(EncodeSMovB32(15, 5));
  computed.push_back(EncodeSop2(0x00, 14, 14, InlineU32(4))); // s_add_u32 s14, s14, 4
  computed.push_back(0xbe8e210eu);
  AppendEnd(&computed);
  const auto partial = ShaderRecompiler::TranslateProgram(computed, options);
  Require(name, "computed target", partial.skip_dispatch && partial.call_targets.empty(),
          "a computed target was reported as a user-data callee");

  // Across control flow: a copy in the entry block reaches a call behind a branch target; a copy
  // on one path only does not.
  std::vector<u32> branchy;
  branchy.push_back(EncodeSMovB32(14, 4));
  branchy.push_back(EncodeSMovB32(15, 5));
  branchy.push_back(EncodeSopp(0x04, 1)); // s_cbranch_scc0 over the next instruction
  branchy.push_back(EncodeSMovB32(20, 6));
  branchy.push_back(0xbe8e210eu); // s_swappc_b64 s[14:15], s[14:15] (a branch target)
  AppendEnd(&branchy);
  const auto dominated = ShaderRecompiler::TranslateProgram(branchy, options);
  Require(name, "entry copy", dominated.skip_dispatch && dominated.call_targets.size() == 1u &&
                                  dominated.call_targets[0].address == 0x0000000200001000ull,
          "an entry-block copy did not reach a call behind a branch target");
  std::vector<u32> one_path;
  one_path.push_back(EncodeSopp(0x04, 2)); // s_cbranch_scc0 over the two copies
  one_path.push_back(EncodeSMovB32(14, 4));
  one_path.push_back(EncodeSMovB32(15, 5));
  one_path.push_back(0xbe8e210eu);
  AppendEnd(&one_path);
  const auto conditional = ShaderRecompiler::TranslateProgram(one_path, options);
  Require(name, "one-path copy", conditional.skip_dispatch && conditional.call_targets.empty(),
          "a copy on one path only was reported as the callee");
  std::printf("[compute] %-32s ok\n", name);
}

void RunAll(VulkanHarness *vulkan) {
  VariantPlanIsFound();
  auto options = GetCodegenOptions();
  options.srt_variant_reads = true;
  ScopedCodegenOptions scope(options);
  CallTargetsFromUserData();
  RunCase(vulkan, ScalarBufferLoadsGpuSelectedDescriptors());
  RunCase(vulkan, ScalarLoadsThroughLoopCarriedRecords());
  vulkan->CheckVariantPlanSkipped();
  vulkan->CheckCallTargetDump();
}

} // namespace SrtVariantTests
