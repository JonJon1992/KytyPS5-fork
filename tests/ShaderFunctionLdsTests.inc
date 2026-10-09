// Compact Function LDS regression, inspired by KytyPS5-Demon f17993c1.
namespace FunctionLdsTests {

u32 FunctionArrayDwords(const std::vector<u32>& words) {
  std::unordered_map<u32, u32> constants, arrays, pointers;
  for (size_t at = 5; at < words.size();) {
    const u32 count = words[at] >> 16u;
    const u32 op = words[at] & 0xffffu;
    Require("FunctionLds", "SPIR-V", count != 0 && at + count <= words.size(), "invalid instruction");
    if (op == spv::OpConstant && count == 4) constants[words[at + 2]] = words[at + 3];
    if (op == spv::OpTypeArray && count == 4) arrays[words[at + 1]] = words[at + 3];
    if (op == spv::OpTypePointer && count == 4 && words[at + 2] == spv::StorageClassFunction)
      pointers[words[at + 1]] = words[at + 3];
    at += count;
  }
  u32 largest = 0;
  for (const auto& [pointer, array] : pointers) {
    if (arrays.contains(array)) largest = std::max(largest, constants.at(arrays.at(array)));
  }
  return largest;
}

GraphicsCase TwoSlots() {
  GraphicsCase test;
  test.name = "PixelFunctionLdsTwoSlots";
  auto& code = test.fragment_code;
  code.push_back(EncodeSMovB32(124, InlineU32(0)));
  AppendVMovLiteral(&code, 0, 0x3e800000u);
  AppendVMovLiteral(&code, 1, 0x3f400000u);
  code.push_back(EncodeDs0(0xb0, 0));
  code.push_back(EncodeDs1(0, 0, 0));
  code.push_back(EncodeDs0(0xb0, 1024));
  code.push_back(EncodeDs1(0, 1, 0));
  code.push_back(EncodeDs0(0xb1, 0));
  code.push_back(EncodeDs1(4, 0, 0));
  code.push_back(EncodeDs0(0xb1, 1024));
  code.push_back(EncodeDs1(5, 0, 0));
  code.push_back(EncodeExp0(0x00, 0xf));
  code.push_back(EncodeExp1(4, 5, 4, 5));
  AppendEnd(&code);
  test.expected_pixel = {0x3e800000u, 0x3f400000u, 0x3e800000u, 0x3f400000u};
  return test;
}

size_t LegacyBoundsChecks(const std::vector<u32>& words) {
  std::unordered_map<u32, u32> constants;
  size_t checks = 0;
  for (size_t at = 5; at < words.size(); at += words[at] >> 16u) {
    const auto op = words[at] & 0xffffu;
    if (op == spv::OpConstant && words[at] >> 16u == 4)
      constants[words[at + 2]] = words[at + 3];
    if (op == spv::OpULessThan && constants.contains(words[at + 4]) &&
        constants.at(words[at + 4]) == 8192u) ++checks;
  }
  return checks;
}

GraphicsCase OffsetCase(u32 offset, const char* name) {
  auto test = GraphicsDsAddtidScratchExport();
  test.name = name;
  for (size_t at = 0; at < test.fragment_code.size(); ++at) {
    auto& word = test.fragment_code[at];
    if ((word >> 26u) == 0x36u) {
      word = (word & 0xffff0000u) | offset;
      ++at; // Skip the operand word.
    }
  }
  test.expected_pixel.clear(); // Edge cases depend on the host's fragment lane.
  return test;
}

GraphicsCase FallbackCase(bool subword) {
  auto test = TwoSlots();
  test.name = subword ? "PixelFunctionLdsSubwordFallback" : "PixelFunctionLdsMixedAddressFallback";
  auto& code = test.fragment_code;
  code.resize(code.size() - 3); // Replace export/end with a constant-address store/load.
  AppendVMovU32(&code, 10, 2048);
  AppendVMovLiteral(&code, 11, subword ? 0u : 0x3f000000u);
  code.push_back(EncodeDs0(0x0d, 0));
  code.push_back(EncodeDs1(0, 11, 10));
  code.push_back(EncodeDs0(subword ? 0x3c : 0x36, 0));
  code.push_back(EncodeDs1(6, 0, 10));
  code.push_back(EncodeExp0(0x00, 0xf));
  code.push_back(EncodeExp1(4, 5, 6, 6));
  AppendEnd(&code);
  test.expected_pixel = {0x3e800000u, 0x3f400000u,
                         subword ? 0u : 0x3f000000u, subword ? 0u : 0x3f000000u};
  return test;
}

void Check(VulkanHarness* vulkan) {
  const std::array cases = {
      TwoSlots(), GraphicsDsAddtidScratchExport(),
      OffsetCase(32768u, "PixelFunctionLdsOutOfBounds"),
      OffsetCase(32764u, "PixelFunctionLdsBoundsEdge"),
      OffsetCase(65532u, "PixelFunctionLdsAddressWrap"),
      FallbackCase(false), FallbackCase(true)};
  const auto saved = ShaderRecompiler::GetCodegenOptions();
  CodegenTests::ScopedCodegenOptions scoped(saved);
  for (bool optimize : {false, true}) {
    for (size_t index = 0; index < cases.size(); ++index) {
      const auto& test = cases[index];
      auto options = saved;
      options.spirv_optimize = optimize;
      options.function_lds_compact = false;
      ShaderRecompiler::SetCodegenOptions(options);
      const auto baseline = CompileFragmentCase(test);
      const auto expected = vulkan->RenderFragment(test, baseline);
      if (!test.expected_pixel.empty()) CompareGraphicsWords(test, expected);
      if (index == 2) {
        Require(test.name, "OOB reference", std::ranges::all_of(expected, [](u32 v) { return v == 0; }),
                "out-of-bounds reads did not return zero");
      }
      options.function_lds_compact = true;
      ShaderRecompiler::SetCodegenOptions(options);
      const auto compact = CompileFragmentCase(test);
      CodegenTests::RequireEqualWords(test.name, "compact readback", expected,
                                     vulkan->RenderFragment(test, compact));
      if (!optimize) {
        const u32 slots = index == 0 ? 2u : index < 5 ? 1u : 8192u;
        Require(test.name, "baseline allocation", FunctionArrayDwords(baseline.spirv) == 8192,
                "test does not exercise the full legacy Function LDS array");
        Require(test.name, "compact allocation", FunctionArrayDwords(compact.spirv) == slots,
                "wrong compact allocation or unsafe fallback");
        Require(test.name, "guest bounds", LegacyBoundsChecks(baseline.spirv) != 0 &&
                    LegacyBoundsChecks(compact.spirv) == LegacyBoundsChecks(baseline.spirv),
                "compaction changed the original 8192-dword bounds checks");
        std::printf("[codegen] %s: Function LDS 8192 -> %u dwords, SPIR-V %zu -> %zu words\n",
                    test.name, slots, baseline.spirv.size(), compact.spirv.size());
      }
    }
  }
  std::printf("[graphics] Function LDS: 7 cases, raw/optimized, original/compact readbacks match\n");
}

} // namespace FunctionLdsTests
