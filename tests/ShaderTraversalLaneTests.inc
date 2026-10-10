// A lane-backed stack, like the traversal in Astro's Playroom CS
// 7d6ab87e6c984bb7 (V_WRITELANE pushes, V_READLANE pops at PC 0x9b4).
// This isolates lane state and loop exits; it does not reproduce the full shader.
namespace TraversalLaneTests {

TestCase StackRoundTrip(u32 depth, uint64_t exec_mask) {
  TestCase test;
  test.name = "TraversalLaneStackRoundTrip";
  test.has_compute_info = true;
  test.compute_info.wave_size = 64;
  test.compute_info.threads_num[0] = 128;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  auto& code = test.code;
  AppendVMovLiteral(&code, 10, 0xdeadbeefu);
  AppendSMovLiteral(&code, 126, static_cast<u32>(exec_mask));
  AppendSMovLiteral(&code, 127, static_cast<u32>(exec_mask >> 32u));
  code.push_back(EncodeSMovB32(20, InlineU32(0)));
  const auto push = code.size();
  code.push_back(EncodeSop2(0x00, 22, 20, InlineU32(1)));
  AppendVop3(&code, 0x361, 10, 22, 20); // writelane ignores EXEC
  code.push_back(EncodeSop2(0x00, 20, 20, InlineU32(1)));
  code.push_back(EncodeSopc(0x07, 20, InlineU32(depth)));
  code.push_back(EncodeSopp(0x05,
      static_cast<u32>(static_cast<int32_t>(push) - static_cast<int32_t>(code.size()) - 1) & 0xffffu));
  code.push_back(EncodeSMovB32(24, InlineU32(0)));
  const auto pop = code.size();
  code.push_back(EncodeSop2(0x01, 20, 20, InlineU32(1)));
  AppendVop3(&code, 0x360, 23, Vgpr(10), 20); // readlane ignores EXEC
  code.push_back(EncodeSop2(0x00, 24, 24, 23));
  code.push_back(EncodeSopc(0x07, 20, InlineU32(0)));
  code.push_back(EncodeSopp(0x05,
      static_cast<u32>(static_cast<int32_t>(pop) - static_cast<int32_t>(code.size()) - 1) & 0xffffu));
  AppendSMovLiteral(&code, 126, 0xffffffffu);
  AppendSMovLiteral(&code, 127, 0xffffffffu);
  code.push_back(EncodeVop1(0x01, 2, 24));
  AppendStoreVgprAtLaneDwordOffset(&code, 2, 0, 0);
  AppendEnd(&code);
  // Independent checksum of the values pushed: 1+...+32 or 1+...+64.
  test.expected.assign(128, depth == 32 ? 528u : 2080u);
  test.required_spirv = {"OpLoopMerge", "OpGroupNonUniformShuffle"};
  return test;
}

void Check(VulkanHarness* vulkan) {
  for (const u32 depth : {32u, 64u}) {
    for (const uint64_t mask : {uint64_t{0}, uint64_t{0xffffffff00000000}, ~uint64_t{0}}) {
      std::printf("[compute] lane stack depth=%u EXEC=%016" PRIx64 "\n", depth, mask);
      RunCase(vulkan, StackRoundTrip(depth, mask));
    }
  }
}

// Characterize the sentinel loop at Astro PC 0x50c..0x5a8 independently of
// BVH intersection. A missing page (or a short V# bound) reads zero, which is
// not the negative list terminator. The fixture's separate 32-entry cap keeps
// this experiment finite; the guest shader has no such counter at this loop.
// This is evidence of a possible liveness failure, not proof of the game's hang.
TestCase SentinelList(bool resident, bool complete_bound, bool continue_read = false) {
  constexpr uint64_t GuestBase = 0x0000000118000000ull;
  constexpr u32 PageBytes = BufferCache::CACHING_PAGESIZE;
  constexpr uint64_t List = GuestBase + PageBytes;
  TestCase test;
  test.name = !resident ? "TraversalListMissingPage" :
              !complete_bound ? "TraversalListShortBound" : "TraversalListResident";
  test.initial.resize(PageBytes * 2 / sizeof(u32));
  const std::array<u32, 4> descriptor{u32(List), u32(List >> 32) | (8u << 16),
                                     complete_bound ? 3u : 2u, 0x16204u};
  std::copy(descriptor.begin(), descriptor.end(), test.initial.begin() + 128);
  const std::array<u32, 6> list{0x10, 0, 0x18, 1, 0x20, 0x80000000u};
  std::copy(list.begin(), list.end(), test.initial.begin() + PageBytes / sizeof(u32));
  test.bda_mappings = {{GuestBase, 0, resident ? PageBytes * 2 : PageBytes}};
  auto& code = test.code;
  // Force a GPU-selected descriptor, like the instance V# in the real shader.
  AppendVMovU32(&code, 30, 64 * sizeof(u32));
  AppendBufferLoadDword(&code, 0, 30);
  code.push_back(EncodeVop1(0x02, 28, Vgpr(0)));
  code.push_back(EncodeSmem0(0x0a, 8, 0));
  code.push_back(EncodeSmem1(512, 28));
  code.push_back(EncodeSMovB32(20, InlineU32(0))); // byte offset
  code.push_back(EncodeSMovB32(21, InlineU32(0))); // fixture safety counter
  code.push_back(EncodeSMovB32(22, InlineU32(0))); // found sentinel
  const auto load_pair = [&] {
    code.push_back(EncodeSmem0(0x09, 24, 4)); // s_buffer_load_dwordx2 s[24:25], s[8:11]
    code.push_back(EncodeSmem1(0, 20));
  };
  if (continue_read) load_pair();
  const auto loop = code.size();
  if (!continue_read) load_pair();
  code.push_back(EncodeSop2(0x00, 21, 21, InlineU32(1)));
  code.push_back(EncodeSopc(0x04, 25, InlineU32(0))); // s_cmp_lt_i32 metadata, 0
  code.push_back(EncodeSop2(0x0a, 22, InlineU32(1), InlineU32(0))); // found from real marker
  const auto found = code.size();
  code.push_back(EncodeSopp(0x05, 0));
  code.push_back(EncodeSopc(0x0a, 21, InlineU32(32)));
  const auto capped = code.size();
  code.push_back(EncodeSopp(0x04, 0));
  code.push_back(EncodeSop2(0x00, 20, 20, InlineU32(8)));
  if (continue_read) load_pair();
  code.push_back(EncodeSopp(0x02,
      u32(static_cast<int32_t>(loop) - static_cast<int32_t>(code.size()) - 1) & 0xffffu));
  code[found] = EncodeSopp(0x05, u32(code.size() - found - 1));
  code[capped] = EncodeSopp(0x04, u32(code.size() - capped - 1));
  AppendStoreSgpr(&code, 21, 0);
  AppendStoreSgpr(&code, 22, 1);
  AppendStoreSgpr(&code, 25, 2);
  AppendEnd(&code);
  test.expected = resident && complete_bound ? std::vector<u32>{3, 1, 0x80000000u}
                                            : std::vector<u32>{32, 0, 0};
  test.required_spirv = {"OpLoopMerge", "OpConvertUToPtr", "PhysicalStorageBuffer"};
  return test;
}

void CheckSentinelLists(VulkanHarness* vulkan) {
  auto options = ShaderRecompiler::GetCodegenOptions();
  options.srt_variant_reads = true;
  CodegenTests::ScopedCodegenOptions scope(options);
  RunCase(vulkan, SentinelList(true, true));
  RunCase(vulkan, SentinelList(false, true));
  RunCase(vulkan, SentinelList(true, false));
}

void CheckScalarReadProbe(VulkanHarness* vulkan) {
  constexpr uint64_t Hash = 0x12345678abcdef01ull;
  for (const auto mode : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) {
    // Missing page, truncated descriptor, healthy, wrong PC, wrong shader,
    // disabled, address outside the guest aperture, and the same three
    // residency/bounds cases with the probed read in the continue construct.
    const bool resident = mode == 1 || mode == 2 || mode == 8 || mode == 9;
    auto test = SentinelList(resident, mode != 1 && mode != 9, mode >= 7);
    if (mode == 6) test.initial[129] = 0x8ffffu;
    test.shader_hash = Hash;
    auto options = ShaderRecompiler::GetCodegenOptions();
    options.srt_variant_reads = true;
    options.scalar_read_probe_shader = mode == 5 ? 0 : mode == 4 ? Hash + 1 : Hash;
    u32 pc = 0;
    for (size_t i = 0; i < test.code.size(); ++i)
      if (test.code[i] == EncodeSmem0(0x09, 24, 4)) pc = u32(i * 4);
    Require(test.name, "probe PC", pc != 0, "list load was not found");
    options.scalar_read_probe_pcs = {mode == 3 ? pc + 4 : pc};
    const bool fault = mode < 2 || mode == 6 || mode == 7 || mode == 9;
    // The diagnostic exits on the next existing loop-exit test, without
    // inventing a sentinel. The first continue read follows one loop iteration.
    if (fault) test.expected = {mode == 7 ? 2u : resident ? 3u : 1u, 0, 0};
    test.check_shader_trap = [=](const ShaderTrapRecord& trap) {
      Require(test.name, "probe claim", trap.claimed == (fault ? 1u : 0u),
              "missing or unexpected diagnostic record");
      if (!fault) return;
      const uint64_t address = uint64_t(trap.address_high) << 32 | trap.address_low;
      const uint64_t size = uint64_t(trap.size_high) << 32 | trap.size_low;
      // Only the second dword (the terminator) is consumed by this fixture;
      // dead-code elimination removes the unused first dword before codegen.
      const uint64_t base = mode == 6 ? 0xffff18004000ull : 0x118004000ull;
      const u32 offset = resident ? 20 : mode == 7 ? 12 : 4;
      Require(test.name, "probe payload",
              trap.code == ShaderTrapRecord::ScalarReadFailure && trap.pc == pc &&
              trap.shader_hash_low == u32(Hash) && trap.shader_hash_high == u32(Hash >> 32) &&
              trap.reason == (resident ? 1u : mode == 6 ? 2u : 3u) &&
              address == base + offset && trap.offset == offset && size == (resident ? 16 : 24) &&
              trap.descriptor[0] == 0x18004000 && trap.descriptor[1] == (mode == 6 ? 0x8ffffu : 0x80001u) &&
              trap.descriptor[2] == (resident ? 2u : 3u) && trap.descriptor[3] == 0x16204 &&
              trap.bda_low == 0 && trap.bda_high == 0,
              "scalar-read fault PC/address/descriptor/bound/status is incorrect");
    };
    CodegenTests::ScopedCodegenOptions scope(options);
    RunCase(vulkan, test);
    std::printf("[compute] scalar read probe case=%d ok\n", mode);
  }
}

} // namespace TraversalLaneTests
