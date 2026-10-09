// Opcode/selector cases adapted from AnyPS5 commit
// 72cf6c3c5a299f645f74a13945670ba4fb8b3cb8,
// core/libs/prx/libSceAgcDriver/tests/execution/Vop3Integer16TernaryOpSel.cpp.
// Runtime inputs and an independent sorted-order oracle exercise the full GPU path.
TestCase Integer16TernarySelectors(u32 operation) {
  constexpr std::array<u32, 6> encodings{0x352, 0x353, 0x355, 0x356, 0x358, 0x359};
  constexpr std::array<const char *, 6> names{
      "V_MIN3_I16", "V_MIN3_U16", "V_MAX3_I16", "V_MAX3_U16",
      "V_MED3_I16", "V_MED3_U16"};
  constexpr std::array<std::array<u32, 3>, 4> rows{{
      {0x4000fffeu, 0x1eede1dcu, 0xc1dc7ed2u}, // Donor captured input row.
      {0x80007fffu, 0x0001ffffu, 0xffff0000u}, // Signed/unsigned extremes.
      {0x80008000u, 0x7fff7fffu, 0x00000000u},
      {0xffff0001u, 0x0001ffffu, 0xffff0001u}, // Ties in both halves.
  }};
  TestCase test;
  test.name = names[operation];
  for (const auto &row : rows) {
    test.initial.insert(test.initial.end(), row.begin(), row.end());
  }
  test.expected = test.initial;
  auto &code = test.code;
  for (u32 row_index = 0; row_index < rows.size(); ++row_index) {
    for (u32 selectors = 0; selectors < 16; ++selectors) {
      const auto &row = rows[row_index];
      for (u32 source = 0; source < 3; ++source) {
        AppendVMovU32(&code, 30, (row_index * 3 + source) * 4);
        AppendBufferLoadDword(&code, 4 + source, 30);
      }
      // One row for each destination alias, plus an independent destination.
      const u32 destination = row_index == 0 ? 1 : 3 + row_index;
      const u32 previous = row_index == 0 ? 0xa5a5beefu : row[row_index - 1];
      if (row_index == 0) AppendVMovLiteral(&code, destination, previous);
      AppendVop3(&code, encodings[operation], destination, Vgpr(4), Vgpr(5),
                 Vgpr(6), 0, selectors);
      AppendStoreVgpr(&code, destination, static_cast<u32>(test.expected.size()));

      std::array<int32_t, 3> sorted{};
      for (u32 source = 0; source < 3; ++source) {
        const u32 half = (row[source] >> (((selectors >> source) & 1u) * 16)) & 0xffffu;
        sorted[source] = static_cast<int32_t>(half);
        if ((operation % 2) == 0 && half >= 0x8000u) sorted[source] -= 0x10000;
      }
      std::sort(sorted.begin(), sorted.end());
      const u32 index = operation < 2 ? 0 : operation < 4 ? 2 : 1;
      const u32 result = static_cast<u32>(sorted[index]) & 0xffffu;
      test.expected.push_back((selectors & 8u) != 0
                                  ? (previous & 0xffffu) | (result << 16)
                                  : (previous & 0xffff0000u) | result);
    }
  }
  AppendEnd(&code);
  test.decoded_counts = {{names[operation], 64}};
  test.required_spirv = {operation == 2 ? "OpSGreaterThan" :
                         operation == 3 ? "OpUGreaterThan" :
                         (operation % 2) == 0 ? "OpSLessThan" : "OpULessThan"};
  return test;
}
