// Fixed input/output pairs from AnyPS5 72cf6c3c5a299f645f74a13945670ba4fb8b3cb8,
// core/libs/prx/libSceAgcDriver/tests/execution/Vop1Vop2Integer.cpp (columns 17/19).
TestCase MulHi24CapturedAndEdges(bool sign) {
  struct Row { u32 lhs, rhs, signed_high, unsigned_high; };
  constexpr std::array<Row, 8> captured{{
      {0x3effffffu, 0x00000005u, 0xffffffffu, 0x00000000u},
      {0x4affffffu, 0x00ffffffu, 0x00000000u, 0x0000ffffu},
      {0x7fc00000u, 0x00800000u, 0x00002000u, 0x00006000u},
      {0xffc00000u, 0x00000020u, 0xffffffffu, 0x00000000u},
      {0x58457b3au, 0xa7913051u, 0xffffe1ecu, 0x00002767u},
      {0x39a77c00u, 0xc0200000u, 0xfffff4efu, 0x000014efu},
      {0x83f73f16u, 0xa7abe1c2u, 0x000002e0u, 0x0000a601u},
      {0x3d932289u, 0x31dec4f4u, 0x00000e21u, 0x00008009u},
  }};
  constexpr std::array<std::array<u32, 2>, 8> edges{{
      {0x00800000u, 0x00800000u}, {0x007fffffu, 0x007fffffu},
      {0x00800000u, 0x007fffffu}, {0x00ffffffu, 0x00ffffffu},
      {0xff000000u, 0x00800000u}, {0xff800000u, 0xaa7fffffu},
      {0x00800000u, 0x00000001u}, {0x00ffffffu, 0x00000001u},
  }};
  const auto expectedHigh = [sign](u32 lhs, u32 rhs) {
    const auto extend = [sign](u32 value) -> int64_t {
      const u32 low = value & 0xffffffu;
      return sign && low >= 0x800000u ? static_cast<int64_t>(low) - 0x1000000
                                     : static_cast<int64_t>(low);
    };
    // Both products fit int64_t; conversion before shifting defines negative cases.
    return static_cast<u32>(static_cast<uint64_t>(extend(lhs) * extend(rhs)) >> 32);
  };
  TestCase test;
  test.name = sign ? "V_MUL_HI_I32_I24" : "V_MUL_HI_U32_U24";
  std::vector<u32> expected;
  for (const auto &row : captured) {
    const u32 fixed = sign ? row.signed_high : row.unsigned_high;
    Require(test.name, "donor oracle", expectedHigh(row.lhs, row.rhs) == fixed,
            "independent 24-bit multiplication disagrees with donor fixture");
    test.initial.insert(test.initial.end(), {row.lhs, row.rhs});
    expected.push_back(fixed);
  }
  for (const auto &row : edges) {
    test.initial.insert(test.initial.end(), row.begin(), row.end());
    expected.push_back(expectedHigh(row[0], row[1]));
  }
  test.expected = test.initial;
  auto &code = test.code;
  for (u32 row = 0; row < expected.size(); ++row) {
    for (u32 encoding = 0; encoding < 4; ++encoding) {
      for (u32 source = 0; source < 2; ++source) {
        AppendVMovU32(&code, 30, (row * 2 + source) * 4);
        AppendBufferLoadDword(&code, 4 + source, 30);
      }
      u32 destination = 4;
      const u32 opcode = sign ? 0x0a : 0x0c;
      if (encoding == 0) {
        // Exact instruction words from the donor fixture, with runtime operands.
        code.push_back(sign ? 0x14360b04u : 0x183a0b04u);
        destination = sign ? 27 : 29;
      } else if (encoding == 1) {
        code.push_back(EncodeVop2(opcode, destination, Vgpr(4), 5));
      } else if (encoding == 2) {
        destination = 5; // Destination aliases the second source.
        AppendVop3(&code, 0x100 + opcode, destination, Vgpr(4), Vgpr(5));
      } else {
        // Literal first source; second source is still loaded at runtime.
        code.push_back(EncodeVop2(opcode, destination, 0xff, 5));
        code.push_back(test.initial[row * 2]);
      }
      AppendStoreVgpr(&code, destination, static_cast<u32>(test.expected.size()));
      test.expected.push_back(expected[row]);
    }
  }
  AppendEnd(&code);
  test.decoded_counts = {{test.name, 64}};
  return test;
}
