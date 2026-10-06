// Exercise the actual BDA load on noncontiguous backing pages, including absent tails.
TestCase GlobalWideLoadCase(u32 count, u32 offset, bool first_present, bool second_present,
                           uint64_t guest_base = 0x0000000110000000ull) {
  constexpr u32 Page = 16384;
  constexpr std::array<u32, 4> Values{0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u};
  TestCase test;
  test.name = count == 2 ? "GlobalWide2" : count == 3 ? "GlobalWide3" : "GlobalWide4";
  test.initial.resize(Page * 4 / sizeof(u32));
  auto* bytes = reinterpret_cast<uint8_t*>(test.initial.data());
  for (u32 component = 0; component < count; ++component) {
    for (u32 byte = 0; byte < 4; ++byte) {
      const u32 guest_offset = offset + component * 4 + byte;
      const u32 backing_offset = guest_offset < Page ? Page + guest_offset
                                                    : 3 * Page + guest_offset - Page;
      bytes[backing_offset] = static_cast<uint8_t>(Values[component] >> (byte * 8));
    }
  }
  test.expected = test.initial;
  for (u32 component = 0; component < count; ++component) {
    test.expected[component] = 0;
    for (u32 byte = 0; byte < 4; ++byte) {
      const bool present = offset + component * 4 + byte < Page ? first_present : second_present;
      if (present) test.expected[component] |= Values[component] & (0xffu << (byte * 8));
    }
  }
  if (first_present) test.bda_mappings.push_back({guest_base, Page, Page});
  if (second_present) test.bda_mappings.push_back({guest_base + Page, 3 * Page, Page});
  // Keep the harness page table enabled even when both accessed pages are absent.
  test.bda_mappings.push_back({guest_base + 2 * Page, 2 * Page, Page});
  AppendVMovLiteral(&test.code, 20, static_cast<u32>(guest_base + offset));
  AppendVMovLiteral(&test.code, 21, static_cast<u32>((guest_base + offset) >> 32));
  test.code.push_back(EncodeFlat0(count == 2 ? 0x0d : count == 3 ? 0x0f : 0x0e, 2));
  test.code.push_back(EncodeFlat1(0, 0x7d, 0, 20));
  for (u32 i = 0; i < count; ++i) AppendStoreVgpr(&test.code, i, i);
  AppendEnd(&test.code);
  return test;
}

void CheckGlobalWideLoads(VulkanHarness* vulkan) {
  for (u32 count : {2u, 3u, 4u}) {
    // The first four cover the common path and all within-page unaligned slow paths.
    for (u32 offset : {32u, 33u, 34u, 35u, 16384u - count * 4u}) {
      RunCase(vulkan, GlobalWideLoadCase(count, offset, true, true));
    }
    RunCase(vulkan, GlobalWideLoadCase(count, 16380, true, true));
    RunCase(vulkan, GlobalWideLoadCase(count, 16380, true, false));
    RunCase(vulkan, GlobalWideLoadCase(count, 16380, false, true));
    RunCase(vulkan, GlobalWideLoadCase(count, 16380, false, false));
    RunCase(vulkan, GlobalWideLoadCase(count, 32, false, false));
    for (u32 offset : {16381u, 16382u, 16383u}) {
      RunCase(vulkan, GlobalWideLoadCase(count, offset, true, true));
      RunCase(vulkan, GlobalWideLoadCase(count, offset, true, false));
      RunCase(vulkan, GlobalWideLoadCase(count, offset, false, true));
    }
    RunCase(vulkan, GlobalWideLoadCase(count, 16380, true, true, 0x00000001ffffc000ull));
  }
}

TestCase GlobalScalarLoadCase(u32 bits, u32 offset, bool first_present, bool second_present,
                              bool active = true) {
  constexpr uint64_t Guest = 0x0000000110000000ull;
  auto test = GlobalWideLoadCase(2, offset, first_present, second_present, Guest);
  test.name = bits == 8 ? "GlobalScalar8" : bits == 16 ? "GlobalScalar16" : "GlobalScalar32";
  test.expected[0] &= bits == 8 ? 0xffu : bits == 16 ? 0xffffu : 0xffffffffu;
  test.expected[1] = test.initial[1];
  test.code.clear();
  AppendVMovLiteral(&test.code, 20, static_cast<u32>(Guest + offset));
  AppendVMovLiteral(&test.code, 21, static_cast<u32>((Guest + offset) >> 32));
  if (!active) {
    AppendVMovLiteral(&test.code, 0, 0xaa551234u);
    AppendSMovLiteral(&test.code, 126, 0);
    AppendSMovLiteral(&test.code, 127, 0);
    test.expected[0] = 0xaa551234u;
  }
  test.code.push_back(EncodeFlat0(bits == 8 ? 0x08 : bits == 16 ? 0x0a : 0x0c, 2));
  test.code.push_back(EncodeFlat1(0, 0x7d, 0, 20));
  if (!active) {
    AppendSMovLiteral(&test.code, 126, 0xffffffffu);
    AppendSMovLiteral(&test.code, 127, 0xffffffffu);
  }
  AppendStoreVgpr(&test.code, 0, 0);
  AppendEnd(&test.code);
  return test;
}

void CheckGlobalScalarLoads(VulkanHarness* vulkan) {
  for (u32 bits : {8u, 16u, 32u}) {
    for (u32 offset : {32u, 33u, 34u, 35u, 16381u, 16382u, 16383u}) {
      RunCase(vulkan, GlobalScalarLoadCase(bits, offset, true, true));
      RunCase(vulkan, GlobalScalarLoadCase(bits, offset, true, false));
      RunCase(vulkan, GlobalScalarLoadCase(bits, offset, false, true));
    }
    RunCase(vulkan, GlobalScalarLoadCase(bits, 16383u, false, false));
    RunCase(vulkan, GlobalScalarLoadCase(bits, 16383u, false, false, false));
  }
}
