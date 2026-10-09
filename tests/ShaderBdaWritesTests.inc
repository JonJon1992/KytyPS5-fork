// KYTY_BDA_WRITES_SHADERS GPU tests (included by ShaderRecompilerComputeTests.cpp after
// ShaderSrtVariantTests.inc).
//
// Ghost of Yotei's "Water Lighting" (CS 0x86da5eb7b8257bb0) and "Shadow Resolve"
// (0xd8959888aafd2552) compute shaders store DWORDs through V#s they select at runtime from a
// user-data table. Resource tracking cannot bind such a V#, so the whole program was skipped. With
// the shader listed, a raw DWORD x1-x4 store through it writes through BDA with the RDNA2 range
// checks of the BDA loads (ISA 8.1.5) and marks its caching page in the fault buffer's written-page
// bitmap for the renderer's settle. The tests' shaders have hash 0.

namespace BdaWritesTests {

using CodegenTests::ScopedCodegenOptions;
using ShaderRecompiler::GetCodegenOptions;

constexpr uint64_t GuestBase = 0x0000000110000000ull;
constexpr uint64_t GuestPage = GuestBase >> BufferCache::CACHING_PAGEBITS;

// V# table entry `entry` (120-byte entries at byte 520, as in BufferLoadsGpuSelectedDescriptors)
// selected through a GPU load of dword `select_dword` and V_READFIRSTLANE into s[8:11], so the
// descriptor is a runtime value. Clobbers v30, v`scratch_vgpr` and s20.
void AppendSelectDescriptor(std::vector<u32> *code, u32 select_dword, u32 scratch_vgpr) {
  AppendVMovU32(code, 30, select_dword * 4u);
  AppendBufferLoadDword(code, scratch_vgpr, 30);
  code->push_back(EncodeVop1(0x02, 20, Vgpr(scratch_vgpr))); // v_readfirstlane_b32 s20
  code->push_back(EncodeSop2(0x26, 20, 20, 255));            // s_mul_i32 s20, s20, 120
  code->push_back(120);
  code->push_back(EncodeSmem0(0x0a, 8, 0)); // s_buffer_load_dwordx4 s[8:11], s[0:3], 520 + s20
  code->push_back(EncodeSmem1(520, 20));
}

void PutDescriptor(TestCase *test, u32 entry, const std::array<u32, 4> &descriptor) {
  std::copy(descriptor.begin(), descriptor.end(), test->initial.begin() + 130 + entry * 30);
}

// BUFFER_STORE_DWORDX4 at offset 0 and BUFFER_STORE_DWORDX2 at offset 16, index 1, through each of
// the descriptor shapes BufferLoadsGpuSelectedDescriptors reads: the stored components land exactly
// where those loads read in range (strides, OOB_SELECT 0-3, SOFFSET, swizzle, an unbound format),
// and nothing else changes.
TestCase StoresGpuSelectedDescriptors() {
  using O = ShaderOpcode;
  struct DescriptorCase {
    u32 stride, records, mode, soffset;
    bool swizzle, bound;
    std::array<u32, 6> dwords; // dword+1 inside the data area of each component, 0: out of range
  };
  const DescriptorCase cases[] = {
      {24, 2, 0, 0, false, true, {7, 8, 9, 10, 11, 12}},
      {16, 2, 0, 0, false, true, {5, 6, 7, 8, 0, 0}},
      {12, 2, 0, 0, false, true, {4, 5, 6, 0, 0, 0}},
      {12, 2, 1, 0, false, true, {4, 5, 6, 7, 8, 9}},
      {12, 1, 2, 0, false, true, {4, 5, 6, 7, 8, 9}},
      {12, 0, 2, 0, false, true, {}},
      {12, 20, 3, 4, false, true, {5, 6, 7, 8, 0, 0}},
      {12, 2, 0, 0, false, false, {}},
      {12, 2, 0, 4, true, true, {3, 11, 19, 0, 0, 0}},
      {12, 8, 3, 4, true, true, {3, 11, 19, 0, 0, 0}},
      {12, 4, 3, 4, true, true, {}},
  };
  constexpr u32 count = static_cast<u32>(std::size(cases));
  const auto value = [](u32 iteration, u32 component) {
    return 0xa0000000u | (iteration << 8u) | component;
  };
  TestCase test;
  test.name = "BdaWritesStoresGpuSelectedDescriptors";
  test.initial.resize(2048);
  for (u32 i = 0; i < count; ++i) {
    const auto &input = cases[i];
    const u32 data_offset = 4096 + i * 128;
    PutDescriptor(&test, i,
                  {static_cast<u32>(GuestBase + data_offset),
                   (input.stride << 16u) | (input.swizzle ? 1u << 31u : 0u) | 1u, input.records,
                   (input.bound ? 0x5204u : 0x204u) | (input.mode << 28u)});
    for (u32 word = 0; word < 32; ++word) {
      test.initial[data_offset / 4 + word] = 0xdead0000u | (i << 8u) | word;
    }
  }
  test.expected = test.initial;
  for (u32 i = 0; i < count; ++i) {
    // Reverse the table order through a GPU load, as the load test does.
    const u32 selected = count - 1 - i;
    test.initial[64 + i] = selected;
    test.expected[64 + i] = selected;
    AppendSelectDescriptor(&test.code, 64 + i, 0);
    AppendSMovLiteral(&test.code, 22, cases[selected].soffset);
    AppendVMovU32(&test.code, 21, 1);
    for (u32 component = 0; component < 6; ++component) {
      AppendVMovU32(&test.code, component, value(i, component));
      const u32 dword = cases[selected].dwords[component];
      if (dword != 0) {
        test.expected[(4096 + selected * 128) / 4 + dword - 1] = value(i, component);
      }
    }
    test.code.push_back(EncodeMubuf0(0x1e, 0, true, false)); // buffer_store_dwordx4 v[0:3]
    test.code.push_back(EncodeMubuf1(0, 2, 21, 22));
    test.code.push_back(EncodeMubuf0(0x1d, 16, true, false)); // buffer_store_dwordx2 v[4:5]
    test.code.push_back(EncodeMubuf1(4, 2, 21, 22));
  }
  AppendEnd(&test.code);
  test.bda_mappings = {{GuestBase, 0}};
  test.expected_bda_written_pages = {GuestPage};
  test.expected_bda_clean_pages = {GuestPage + 1};
  test.expected_bda_dropped_writes = 0;
  test.opcodes = {O::V_MOV_B32, O::S_MOV_B32, O::BUFFER_LOAD_DWORD, O::V_READFIRSTLANE_B32,
                  O::S_MUL_I32, O::S_BUFFER_LOAD_DWORDX4, O::BUFFER_STORE_DWORDX4,
                  O::BUFFER_STORE_DWORDX2, O::S_ENDPGM};
  test.required_spirv = {"OpConvertUToPtr", "PhysicalStorageBuffer", "OpGroupNonUniformAllEqual"};
  return test;
}

// Lanes of one wave store to five caching pages through one runtime V# (index = lane, stride 1 KiB):
// every page's bit is set (the per-lane marking path, the pages differ across the wave), the next
// page stays clear. A second V# addresses an unmapped page: all 64 stores are dropped and counted,
// and that page is not marked.
TestCase StoresMarkEveryWrittenPage() {
  using O = ShaderOpcode;
  constexpr u32 lanes = 64;
  constexpr u32 data_offset = 0x2000;
  constexpr u32 stride = 1024;
  constexpr uint64_t unmapped = GuestBase + (uint64_t{64} << BufferCache::CACHING_PAGEBITS);
  TestCase test;
  test.name = "BdaWritesStoresMarkEveryWrittenPage";
  test.initial.assign((data_offset + lanes * stride) / 4, 0x5e5e5e5eu);
  PutDescriptor(&test, 0,
                {static_cast<u32>(GuestBase + data_offset), (stride << 16u) | 1u, lanes, 0x5204u});
  PutDescriptor(&test, 1,
                {static_cast<u32>(unmapped), (stride << 16u) | static_cast<u32>(unmapped >> 32u),
                 lanes, 0x5204u});
  test.initial[64] = 0;
  test.initial[65] = 1;
  test.expected = test.initial;
  for (u32 lane = 0; lane < lanes; ++lane) {
    test.expected[(data_offset + lane * stride) / 4] = 0xb0000000u + lane;
  }
  // v0 is the lane's local id. v2 = 0xb0000000 + lane.
  test.code.push_back(EncodeVop2(0x25, 2, 255, 0)); // v_add_nc_u32 v2, literal, v0
  test.code.push_back(0xb0000000u);
  for (u32 entry = 0; entry < 2; ++entry) {
    AppendSelectDescriptor(&test.code, 64 + entry, 1);
    test.code.push_back(EncodeMubuf0(0x1c, 0, true, false)); // buffer_store_dword v2, v0 idxen
    test.code.push_back(EncodeMubuf1(2, 2, 0));
  }
  AppendEnd(&test.code);
  test.compute_info.threads_num[0] = lanes;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.wave_size = 64;
  test.has_compute_info = true;
  test.bda_mappings = {{GuestBase, 0}};
  for (uint64_t page = 0; page < 5; ++page) {
    test.expected_bda_written_pages.push_back(GuestPage + page);
  }
  test.expected_bda_clean_pages = {GuestPage + 5, unmapped >> BufferCache::CACHING_PAGEBITS};
  test.expected_bda_dropped_writes = lanes;
  test.opcodes = {O::V_ADD_NC_U32, O::V_MOV_B32, O::BUFFER_LOAD_DWORD, O::V_READFIRSTLANE_B32,
                  O::S_MUL_I32, O::S_BUFFER_LOAD_DWORDX4, O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  return test;
}

// Without the shader in KYTY_BDA_WRITES_SHADERS the store keeps dropping the program; listed, it
// translates with ShaderInfo::bda_writes and address writes (the unbounded-writer handling).
void ListedShadersOnly() {
  const char *name = "BdaWritesListedShadersOnly";
  const auto test = StoresMarkEveryWrittenPage();
  auto user_data = MakeNativeUserData(nullptr);
  ShaderComputeInputInfo compute_info = test.compute_info;
  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Compute;
  options.input_info.compute = &compute_info;
  options.user_data = user_data;
  options.wave_size = 64;
  auto codegen = GetCodegenOptions();
  codegen.bda_writes_shaders.clear();
  {
    ScopedCodegenOptions scope(codegen);
    const auto unlisted = ShaderRecompiler::TranslateProgram(test.code, options);
    Require(name, "unlisted", unlisted.skip_dispatch,
            "a store through a runtime V# did not drop an unlisted program");
  }
  codegen.bda_writes_shaders = {0};
  ScopedCodegenOptions scope(codegen);
  const auto listed = ShaderRecompiler::TranslateProgram(test.code, options);
  Require(name, "listed",
          !listed.skip_dispatch && listed.program.info.bda_writes &&
              listed.program.has_address_writes && listed.program.info.uses_dma,
          "a listed program was not translated with BDA writes");
  std::printf("[compute] %-32s ok\n", name);
}

void CandidateProofs() {
  constexpr const char* name = "BdaCandidateProofs";
  using namespace BdaWriteCandidates;
  const auto make = [](uint64_t hash, u32 count) {
    Plan p;
    p.shader_hash = hash; p.table = 0x200000000ull;
    p.words[0x70 / 4] = count;
    const auto first = hash == ShaderRecompiler::BdaWaterLightingHash ? 8u : 4u;
    const auto entries = hash == ShaderRecompiler::BdaWaterLightingHash ? 5u : count;
    for (u32 i = 0; i < entries && i < MaxDescriptors; ++i) {
      p.words[first + 4 * i] = 0x100000 + i * 0x10000;
      p.words[first + 4 * i + 1] = 4u << 16;
      p.words[first + 4 * i + 2] = 16;
      p.words[first + 4 * i + 3] = 0x5204;
    }
    p.table_bytes = TableBytes(hash, p.words);
    return p;
  };
  auto water = make(ShaderRecompiler::BdaWaterLightingHash, 0);
  Require(name, "water", Resolve(water) && water.range_count == 5 && water.ranges[4].size == 64,
      "clamped five-entry table lost a candidate");
  auto shadow = make(ShaderRecompiler::BdaShadowResolveHash, 6);
  Require(name, "runtime count", Resolve(shadow) && shadow.range_count == 6,
      "shadow count silently clamped to the unproven four-entry layout");
  auto bad = water; bad.words[11] |= 2u << 28;
  Require(name, "unbounded OOB", !Resolve(bad), "OOB_SELECT=2 treated as bounded");
  bad = water; bad.words[11] |= 3u << 28;
  Require(name, "raw OOB index", !Resolve(bad), "OOB_SELECT=3 with a stride admitted unbounded indices");
  bad = water; bad.table |= 1u;
  Require(name, "unaligned table", !Resolve(bad), "host and shader could read different tables");
  bad = water; bad.words[9] |= 1u << 31;
  Require(name, "swizzle", !Resolve(bad), "swizzle admitted without proof");
  bad = water; bad.words[10] = UINT32_MAX;
  Require(name, "capacity", !Resolve(bad), "huge record count escaped the limit");
  bad = water; bad.words[8] = static_cast<u32>(bad.table);
  bad.words[9] = static_cast<u32>(bad.table >> 32) | (4u << 16);
  Require(name, "self modifying", !Resolve(bad), "table self-writes admitted");
  shadow.words[0x70 / 4] = MaxDescriptors + 1;
  Require(name, "no truncation", TableBytes(shadow.shader_hash, shadow.words) == 0,
      "descriptor count truncated");
  auto empty = make(ShaderRecompiler::BdaShadowResolveHash, 0);
  Require(name, "zero count", Resolve(empty) && empty.range_count == 0, "empty table rejected");
  auto off = GetCodegenOptions(); off.bda_write_mode = ShaderRecompiler::BdaWriteMode::Off;
  {
    ScopedCodegenOptions scope(off);
    Require(name, "opt in", !ShaderRecompiler::BdaWriteCandidatesApplies(water.shader_hash),
        "candidates enabled by default");
  }
  auto options = GetCodegenOptions();
  options.bda_write_mode = ShaderRecompiler::BdaWriteMode::Candidates;
  ScopedCodegenOptions scope(options);
  Require(name, "allowlist", !ShaderRecompiler::BdaWriteCandidatesApplies(123),
      "unknown shader allowed to omit settle");
  ProgramDiskCache::SourceKeyInputs inputs{};
  ProgramDiskCache::SourceKey a, b, c;
  inputs.bda_writes = true;
  inputs.bda_write_mode = 0; ProgramDiskCache::BuildSourceKey(inputs, a);
  inputs.bda_write_mode = 1; ProgramDiskCache::BuildSourceKey(inputs, b);
  inputs.bda_write_mode = 2; ProgramDiskCache::BuildSourceKey(inputs, c);
  Require(name, "cache mode", a.bytes != b.bytes && b.bytes != c.bytes && a.bytes != c.bytes,
      "phase 0, candidates and verification shared a cache key");
  std::printf("[compute] %-32s ok\n", name);
}

// dropped: the V# points at a page without a cache buffer, so every lane's write is dropped and
// counted (also without the bitmap: the count is how a candidate dispatch reports a lost write).
TestCase CandidateStore(bool verify, bool dropped = false) {
  using O = ShaderOpcode;
  TestCase test;
  test.name = dropped  ? "BdaCandidatesDroppedCounted"
              : verify ? "BdaCandidatesBitmapReference"
                       : "BdaCandidatesWithoutBitmap";
  test.shader_hash = ShaderRecompiler::BdaWaterLightingHash;
  test.initial.assign(2048, 0);
  const auto destination = GuestBase + (dropped ? 0x40000u : 4096u);
  PutDescriptor(&test, 0, {static_cast<u32>(destination), (4u << 16) | 1u, 64, 0x5204});
  test.expected = test.initial;
  if (!dropped) std::fill_n(test.expected.begin() + 1024, 64, 0xc0ffee42u);
  AppendSelectDescriptor(&test.code, 64, 1);
  AppendVMovU32(&test.code, 2, 0xc0ffee42u);
  while (test.code.size() * 4 < 0x530)
    test.code.push_back(EncodeSMovB32(31, InlineU32(0)));
  test.code.push_back(EncodeMubuf0(0x1c, 0, true, false));
  test.code.push_back(EncodeMubuf1(2, 2, 0));
  AppendEnd(&test.code);
  test.compute_info.threads_num[0] = 64;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.wave_size = 64;
  test.has_compute_info = true;
  // Only the descriptor's page is mapped when the write must be dropped.
  test.bda_mappings = {{GuestBase, 0, dropped ? 4096u : 0u}};
  test.expected_bda_dropped_writes = dropped ? 64u : 0u;
  if (verify) {
    test.expected_bda_written_pages = {GuestPage};
    test.required_spirv = {"OpGroupNonUniformAllEqual"};
  } else {
    test.expected_bda_clean_pages = {GuestPage};
    test.forbidden_spirv = {"OpGroupNonUniformAllEqual"};
  }
  test.opcodes = {O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  return test;
}

// The count at table+0x70 must remain a GPU scalar read. Resource materialization precedes
// freezing and cannot supply an older count through a flattened SRT slot.
TestCase CandidateTableCount() {
  TestCase test;
  test.name = "BdaCandidatesGpuTableCount";
  test.shader_hash = ShaderRecompiler::BdaShadowResolveHash;
  test.initial.assign(128, 0);
  test.initial[0x70 / 4] = 6;
  test.expected = test.initial;
  test.expected[0] = 6;
  AppendSMovLiteral(&test.code, 20, static_cast<u32>(GuestBase));
  AppendSMovLiteral(&test.code, 21, static_cast<u32>(GuestBase >> 32));
  test.code.push_back(EncodeSmem0(0x00, 22, 10)); // s_load_dword s22, s[20:21], 0x70
  test.code.push_back(EncodeSmem1(0x70, 125));
  test.code.push_back(EncodeSopp(0x0c, 0xc07f));
  AppendStoreSgpr(&test.code, 22, 0);
  AppendEnd(&test.code);
  test.bda_mappings = {{GuestBase, 0}};
  test.opcodes = {ShaderOpcode::S_LOAD_DWORD, ShaderOpcode::BUFFER_STORE_DWORD,
                  ShaderOpcode::S_ENDPGM};
  test.required_spirv = {"PhysicalStorageBuffer"};
  return test;
}

void RunCandidates() {
  auto options = GetCodegenOptions();
  options.srt_variant_reads = true;
  options.bda_writes_shaders.clear();
  options.bda_write_mode = ShaderRecompiler::BdaWriteMode::Candidates;
  ScopedCodegenOptions scope(options);
  CandidateProofs();
  VulkanHarness vulkan;
  RunCase(&vulkan, CandidateStore(false));
  RunCase(&vulkan, CandidateStore(false, true));
  RunCase(&vulkan, CandidateTableCount());
  {
    auto verify = options;
    verify.bda_write_mode = ShaderRecompiler::BdaWriteMode::CandidatesVerify;
    ScopedCodegenOptions check(verify);
    RunCase(&vulkan, CandidateStore(true));
  }
  vulkan.CheckBdaWriteCandidates();
}

void RunAll() {
  auto options = GetCodegenOptions();
  options.srt_variant_reads = true;
  options.bda_writes_shaders = {0};
  ScopedCodegenOptions scope(options);
  ListedShadersOnly();
  // The harness sizes its fault buffer for the written-page bitmap only while the list is set.
  VulkanHarness vulkan;
  RunCase(&vulkan, StoresGpuSelectedDescriptors());
  RunCase(&vulkan, StoresMarkEveryWrittenPage());
  // The renderer's synchronous settle, through the real RenderExecutor and buffer cache.
  vulkan.CheckBdaWritesSettle();
}

} // namespace BdaWritesTests
