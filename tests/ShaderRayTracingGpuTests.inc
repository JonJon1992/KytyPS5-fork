// Adapted from Senaxx/KytyPS5 Wolverine, commit 6b3fbc83d3bf39c89b3bb1adc68b6e50ce66732f (GPL-2.0).
TestCase FlatStackApertures(u32 wave_size) {
  TestCase test;
  test.name = wave_size == 32 ? "FlatStackApertures32" : "FlatStackApertures64";
  auto &code = test.code;
  code = {EncodeSop1(0x04, 8, 237), // private base
          EncodeSop1(0x0b, 11, InlineU32(1)), // S_BREV_B32 shared aperture, as in GTA.
          EncodeVop2(0x1a, 20, InlineU32(2), 0),
          EncodeVop2(0x1b, 22, InlineU32(1), 0),
          EncodeVop2(0x1a, 22, InlineU32(31), 22)};
  AppendVop3(&code, 0x159, 21, 9, 11, Vgpr(22)); // Clamp the stack address to the local apertures.
  AppendVMovU32(&code, 30, 100);
  code.push_back(EncodeVop2(0x25, 30, Vgpr(0), 30));
  code.insert(code.end(), {EncodeFlat0(0x1c, 1), EncodeFlat1(0, 0x7d, 30, 20)});
  AppendVMovU32(&code, 30, 200);
  code.push_back(EncodeVop2(0x25, 30, Vgpr(0), 30));
  code.insert(code.end(), {EncodeDs0(0x0d), EncodeDs1(0, 30, 20)});
  AppendVMovU32(&code, 30, 300);
  code.push_back(EncodeVop2(0x25, 30, Vgpr(0), 30));
  code.insert(code.end(), {EncodeFlat0(0x1c, 0), EncodeFlat1(0, 0x7d, 30, 20)});
  // FLAT writes must alias both explicit memory instructions, per invocation.
  code.insert(code.end(), {EncodeFlat0(0x0c, 1), EncodeFlat1(24, 0x7d, 0, 20),
                           EncodeDs0(0x36), EncodeDs1(25, 0, 20),
                           EncodeFlat0(0x0c, 0), EncodeFlat1(26, 0x7d, 0, 20)});
  AppendStoreVgprAtLaneDwordOffset(&code, 24, 0, 0);
  AppendStoreVgprAtLaneDwordOffset(&code, 25, 0, wave_size);
  AppendStoreVgprAtLaneDwordOffset(&code, 26, 0, 2 * wave_size);
  // Inactive lanes must preserve both stores and destination VGPRs.
  AppendSMovLiteral(&code, 126, 0x55555555);
  if (wave_size == 64) AppendSMovLiteral(&code, 127, 0x55555555);
  code.push_back(EncodeVop2(0x25, 22, InlineU32(1), 22));
  AppendVop3(&code, 0x159, 21, 9, 11, Vgpr(22));
  AppendVMovU32(&code, 30, 400);
  code.push_back(EncodeVop2(0x25, 30, Vgpr(0), 30));
  code.insert(code.end(), {EncodeFlat0(0x1c, 0), EncodeFlat1(0, 0x7d, 30, 20)});
  code.insert(code.end(), {EncodeFlat0(0x0c, 0), EncodeFlat1(26, 0x7d, 0, 20)});
  code.push_back(EncodeSop1(0x04, 126, 193)); // -1 sign-extends across EXEC
  AppendStoreVgprAtLaneDwordOffset(&code, 26, 0, 3 * wave_size);
  // Noncanonical addresses outside either aperture, aperture overflow, and
  // offsets outside allocated local storage must not wrap into valid storage.
  AppendVMovLiteral(&code, 21, 0x78000000);
  code.insert(code.end(), {EncodeFlat0(0x1c, 0), EncodeFlat1(0, 0x7d, 30, 20),
                           EncodeFlat0(0x0c, 0), EncodeFlat1(26, 0x7d, 0, 20)});
  AppendStoreVgprAtLaneDwordOffset(&code, 26, 0, 4 * wave_size);
  code.push_back(EncodeVop1(0x01, 21, 11));
  AppendVMovLiteral(&code, 20, 0xfffffffc);
  code.insert(code.end(), {EncodeFlat0(0x1c, 0, 4), EncodeFlat1(0, 0x7d, 30, 20),
                           EncodeFlat0(0x0c, 0, 4), EncodeFlat1(26, 0x7d, 0, 20)});
  AppendStoreVgprAtLaneDwordOffset(&code, 26, 0, 5 * wave_size);
  AppendVMovU32(&code, 20, wave_size * 4);
  code.insert(code.end(), {EncodeFlat0(0x1c, 0), EncodeFlat1(0, 0x7d, 30, 20),
                           EncodeFlat0(0x0c, 0), EncodeFlat1(26, 0x7d, 0, 20)});
  AppendStoreVgprAtLaneDwordOffset(&code, 26, 0, 6 * wave_size);
  AppendVMovU32(&code, 20, 1);
  code.insert(code.end(), {EncodeFlat0(0x1c, 0), EncodeFlat1(0, 0x7d, 30, 20),
                           EncodeFlat0(0x0c, 0), EncodeFlat1(26, 0x7d, 0, 20)});
  AppendStoreVgprAtLaneDwordOffset(&code, 26, 0, 9 * wave_size);
  code.push_back(EncodeVop2(0x1a, 20, InlineU32(2), 0));
  code.insert(code.end(), {EncodeFlat0(0x0c, 1), EncodeFlat1(24, 0x7d, 0, 20),
                           EncodeDs0(0x36), EncodeDs1(25, 0, 20)});
  AppendStoreVgprAtLaneDwordOffset(&code, 24, 0, 7 * wave_size);
  AppendStoreVgprAtLaneDwordOffset(&code, 25, 0, 8 * wave_size);
  AppendEnd(&code);
  test.expected.resize(10 * wave_size);
  for (u32 lane = 0; lane < wave_size; ++lane) {
    const bool shared = lane & 1;
    test.expected[lane] = (shared ? 100 : 300) + lane;
    test.expected[wave_size + lane] = (shared ? 300 : 200) + lane;
    test.expected[2 * wave_size + lane] = 300 + lane;
    test.expected[3 * wave_size + lane] = (shared ? 300 : 400) + lane;
    test.expected[7 * wave_size + lane] = (shared ? 100 : 400) + lane;
    test.expected[8 * wave_size + lane] = (shared ? 300 : 200) + lane;
  }
  test.compute_info.wave_size = wave_size;
  test.compute_info.threads_num[0] = wave_size;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.scratch_size_dwords = wave_size;
  test.compute_info.lds_size_dwords = wave_size;
  test.has_compute_info = true;
  test.decoded_counts = {{"S_BREV_B32", 1}};
  test.ir_counts = {{" = BitReverse32 ", 0}};
  test.forbidden_spirv = {"get_bda_pointer"};
  return test;
}

// Extended forms follow AMD RDNA2 ISA section 8.2.10 and the operand cases in
// JonJon1992/AnyPS5 c963432d, Bvh64IntersectRay.cpp. Keep our existing geometry
// oracle (including triangle kinds 2/3 and watertight edges).
TestCase BvhIntersections(bool barycentrics, bool sorted, u32 grow = 0,
                          bool node64 = false, bool a16 = false, u32 wave = 32) {
  TestCase test;
  test.name = barycentrics ? (sorted ? "BvhBarycentricSorted" : "BvhBarycentricUnsorted")
                          : (sorted ? "BvhTriangleIdSorted" : "BvhTriangleIdUnsorted");
  if (grow != 0) test.name = "BvhBoxExitGrowth";
  constexpr u32 NodeStart = 1024, OutputStart = 512, LaneCount = 32;
  const uint64_t GuestBase = node64 ? 0x800010000ull : 0x10000ull;
  const uint64_t NodeBias = node64 ? GuestBase >> 3 : 0;
  const uint64_t DescriptorBase = node64 ? 0 : GuestBase;
  const uint64_t LastNode = (NodeBias >> 3) + 256;
  const uint64_t BoundedLastNode = (NodeBias >> 3) + 2;
  static constexpr const char* extended_names[3][4] = {
      {"Bvh32A16IdUnsorted", "Bvh32A16IdSorted", "Bvh32A16BaryUnsorted", "Bvh32A16BarySorted"},
      {"Bvh64IdUnsorted", "Bvh64IdSorted", "Bvh64BaryUnsorted", "Bvh64BarySorted"},
      {"Bvh64A16IdUnsorted", "Bvh64A16IdSorted", "Bvh64A16BaryUnsorted", "Bvh64A16BarySorted"}};
  if (node64 || a16) test.name = extended_names[(node64 ? 2 : 0) + (a16 ? 1 : 0) - 1]
                                                              [(barycentrics ? 2 : 0) + (sorted ? 1 : 0)];
  if (wave == 64) test.name = "Bvh64A16Wave64";
  const auto bits = [](float value) { return std::bit_cast<u32>(value); };
  constexpr u32 Infinity = 0x7f800000, Invalid = UINT32_MAX, Flag = 0x04040404;
  test.initial.resize(NodeStart + 258 * 16);
  auto &data = test.initial;
  // Pack five float3 vertices and the triangle flag into a 64-byte node.
  const std::array<float, 15> vertices{-1, -1, 2, 1, -1, 2, 0, 1, 2, 2, 1, 2, 0, 3, 2};
  for (u32 i = 0; i < vertices.size(); ++i) data[NodeStart + i] = bits(vertices[i]);
  data[NodeStart + 15] = Flag;
  constexpr std::array<u32, 4> children{0x88, 0x90, 0x98, 0xa0};
  constexpr std::array<float, 4> z{5, 1, 3, 7};
  constexpr std::array<u32, 4> half_z{0x4500, 0x3c00, 0x4200, 0x4700};
  constexpr std::array<u32, 4> half_far{0x4600, 0x4000, 0x4400, 0x4800};
  for (u32 child = 0; child < 4; ++child) {
    data[NodeStart + 16 + child] = data[NodeStart + 32 + child] = children[child];
    const auto fp16 = NodeStart + 20 + child * 3;
    data[fp16] = 0xbc00bc00; // min.xy = -1,-1
    data[fp16 + 1] = 0x3c000000 | half_z[child];
    data[fp16 + 2] = (half_far[child] << 16) | 0x3c00;
    const auto fp32 = NodeStart + 36 + child * 6;
    const std::array<float, 6> bounds{-1, -1, z[child], 1, 1, z[child] + 1};
    for (u32 i = 0; i < 6; ++i) data[fp32 + i] = bits(bounds[i]);
  }
  // A grazing ray enters one ULP after exiting on another axis.
  for (u32 child = 0; child < 4; ++child) {
    data[NodeStart + 64 + child] = children[child];
    const std::array<float, 6> bounds = child == 0
        ? std::array<float, 6>{0, 0, std::nextafter(1.0f, 2.0f), 1, 2, 2}
        : child == 1 ? std::array<float, 6>{-1, 1, -1, -0.0f, 2, 2}
                     : std::array<float, 6>{1, 1, 1, -1, -1, -1};
    for (u32 i = 0; i < 6; ++i) data[NodeStart + 68 + child * 6 + i] = bits(bounds[i]);
  }
  // An FP32 node straddles two guest pages with a 64-byte gap in physical backing.
  std::copy_n(data.begin() + NodeStart + 32, 16, data.begin() + NodeStart + 255 * 16);
  std::copy_n(data.begin() + NodeStart + 48, 16, data.begin() + NodeStart + 257 * 16);
  const std::array<std::array<float, 2>, 4> origins{{{0, 0}, {1, 0}, {.5f, 1.5f}, {-.25f, 1}}};
  std::array<std::array<u32, 4>, LaneCount> expected{};
  for (u32 lane = 0; lane < LaneCount; ++lane) {
    const auto kind = lane % 8;
    const auto input = lane * 16;
    data[input] = kind < 4 ? kind : kind == 4 ? 12 : kind == 5 ? 21 : kind == 6 ? 6 : 0x1000;
    data[input + 1] = bits(6);
    data[input + 2] = bits(kind < 4 ? origins[kind][0] : 0);
    data[input + 3] = bits(kind < 4 ? origins[kind][1] : 0);
    data[input + 7] = bits(1);
    data[input + 8] = data[input + 9] = Infinity;
    data[input + 10] = bits(1);
    if (kind < 4) {
      const float denominator = kind == 3 ? -2 : -4;
      const float i = kind == 0 || kind == 3 ? -1 : -2;
      const float j = kind == 3 ? -.5f : -1;
      expected[lane] = {bits(2 * denominator), bits(denominator),
                        barycentrics ? bits(i) : Flag + kind,
                        barycentrics ? bits(j) : 1};
    } else if (kind < 6) {
      expected[lane] = sorted ? std::array<u32, 4>{children[1], children[2], children[0], Invalid}
                              : std::array<u32, 4>{children[0], children[1], children[2], Invalid};
    } else {
      expected[lane] = kind == 6 ? std::array<u32, 4>{Invalid, Invalid, Invalid, Invalid}
                                 : std::array<u32, 4>{Infinity, bits(1), Invalid, 0};
    }
    if (lane >= 8 && lane < 12) {
      data[input + 2] = 0x7fc00000; // invalid ray origin
      expected[lane] = {Infinity, bits(1), Invalid, 0};
    } else if (lane == 12 || lane == 13) {
      data[input + 1] = bits(1); // nearest face equals the exclusive extent
      expected[lane] = {Invalid, Invalid, Invalid, Invalid};
    } else if (lane >= 16 && lane < 20) {
      const u32 axis = lane & 1;
      data[input + 2 + axis] = bits(origins[kind][axis] - 4);
      data[input + 5 + axis] = bits(2);
      data[input + 8 + axis] = bits(.5f);
      // Dominant X/Y projection scales all intersection numerators and the
      // denominator by four; the distance and barycentrics remain unchanged.
      for (u32 i = 0; i < (barycentrics ? 4u : 2u); ++i)
        expected[lane][i] = bits(std::bit_cast<float>(expected[lane][i]) * 4);
    } else if (lane == 22) {
      data[input] = 37; // node 4, FP32
      data[input + 1] = bits(10);
      for (u32 i = 5; i <= 10; ++i) data[input + i] = bits(1);
      expected[lane] = {grow ? children[0] : Invalid, Invalid, Invalid, Invalid};
    } else if (lane >= 24 && lane < 28) {
      data[input + 4] = bits(4);
      data[input + 7] = data[input + 10] = bits(-1);
      for (u32 i = 0; i < (barycentrics ? 4u : 2u); ++i)
        expected[lane][i] = bits(-std::bit_cast<float>(expected[lane][i]));
    } else if (lane == 28) {
      data[input] = 0;
      data[input + 3] = bits(-1); // horizontal triangle edge: top-left rule rejects
      expected[lane] = {Infinity, bits(1), barycentrics ? bits(-2) : Flag,
                        barycentrics ? bits(-2) : 0};
    } else if (lane == 29) {
      data[input] = (255 << 3) | 5;
    }
  }
  // Repack the ray record to the real ISA layout. Only direction and inverse
  // direction become half floats; the fractional origin stays full precision.
  const auto half = [](u32 bits) -> u32 {
    switch (bits) {
      case 0: return 0;
      case 0x3f000000: return 0x3800; // .5
      case 0x3f800000: return 0x3c00; // 1
      case 0xbf800000: return 0xbc00; // -1
      case 0x40000000: return 0x4000; // 2
      case 0x7f800000: return 0x7c00;
      default: Fail("BvhIntersections", "fixture", "unexpected half value");
    }
  };
  for (u32 lane = 0; lane < LaneCount; ++lane) {
    const u32 at = lane * 16;
    std::array<u32, 11> raw;
    std::copy_n(data.begin() + at, raw.size(), raw.begin());
    uint64_t node = NodeBias + raw[0];
    if (node64 && lane == 30) {
      // A large node whose byte shift wraps must never alias mapped geometry.
      node = (1ull << 63) | (NodeBias + 21);
      expected[lane] = {Invalid, Invalid, Invalid, Invalid};
    }
    if (node64 && lane == 31) {
      node = (1ull << 37); // first address beyond the 40-bit guest aperture
      expected[lane] = {Infinity, bits(1), Invalid, 0};
    }
    data[at] = static_cast<u32>(node);
    const u32 ray = node64 ? 2 : 1;
    if (node64) data[at + 1] = static_cast<u32>(node >> 32);
    std::copy_n(raw.begin() + 1, 4, data.begin() + at + ray);
    if (a16) {
      for (u32 i = 0; i < 3; ++i)
        data[at + ray + 4 + i] = half(raw[5 + i * 2]) | (half(raw[6 + i * 2]) << 16);
    } else {
      std::copy_n(raw.begin() + 5, 6, data.begin() + at + ray + 4);
    }
  }
  test.expected = test.initial;
  for (u32 lane = 0; lane < LaneCount; ++lane) {
    std::copy(expected[lane].begin(), expected[lane].end(),
              test.expected.begin() + OutputStart + lane * 4);
    auto bounded = expected[lane];
    if (lane % 8 == 5) bounded = {Invalid, Invalid, Invalid, Invalid};
    if (lane >= 20) bounded = {0x12345678, 0x12345679, 0x1234567a, 0x1234567b};
    std::copy(bounded.begin(), bounded.end(),
              test.expected.begin() + OutputStart + LaneCount * 4 + lane * 4);
  }
  auto &code = test.code;
  // The resource is a GPU value, not a materialized host image descriptor.
  AppendSMovLiteral(&code, 8, DescriptorBase >> 8);
  AppendSMovLiteral(&code, 9, (sorted ? 0x80000000 : 0) | (grow << 23));
  AppendSMovLiteral(&code, 10, static_cast<u32>(LastNode));
  AppendSMovLiteral(&code, 11, (barycentrics ? 0x81000000 : 0x80000000) | static_cast<u32>(LastNode >> 32));
  code.push_back(EncodeVop2(0x1a, 1, InlineU32(6), 0));
  for (u32 offset : {0u, 4u, 8u}) {
    code.insert(code.end(), {EncodeMubuf0(0x0e, offset * 4),
                             EncodeMubuf1(4 + offset, 0, 1)});
  }
  code.insert(code.end(), {EncodeMimg0(node64 ? 0xe7 : 0xe6, 15, 0, false, 0, true),
                           EncodeMimg1(20, 4, 2) | (a16 ? 1u << 30 : 0u)});
  code.push_back(EncodeVop2(0x1a, 2, InlineU32(2), 0));
  for (u32 i = 0; i < 4; ++i) AppendStoreVgprAtLaneDwordOffset(&code, 20 + i, 2, OutputStart + i);
  // NSA addressing, the FP32 second-block bound, and inactive destination lanes.
  AppendSMovLiteral(&code, 10, static_cast<u32>(BoundedLastNode));
  for (u32 i = 0; i < 4; ++i) AppendVMovLiteral(&code, 24 + i, 0x12345678 + i);
  AppendSMovLiteral(&code, 126, 0x000fffff);
  const u32 components = (a16 ? 8u : 11u) + (node64 ? 1u : 0u);
  const u32 nsa_words = (components - 1 + 3) / 4;
  // Reverse the operand registers so NSA cannot accidentally pass by falling
  // back to contiguous addressing. v40 contains operand zero, v39 operand one.
  for (u32 i = 0; i < components; ++i)
    code.push_back(EncodeVop1(0x01, 40 - i, Vgpr(4 + i)));
  code.insert(code.end(), {EncodeMimg0(node64 ? 0xe7 : 0xe6, 15, nsa_words, false, 0, true),
                           EncodeMimg1(24, 40, 2) | (a16 ? 1u << 30 : 0u)});
  for (u32 word = 0; word < nsa_words; ++word) {
    u32 addresses = 0;
    for (u32 byte = 0; byte < 4; ++byte) addresses |= (39 - word * 4 - byte) << (byte * 8);
    code.push_back(addresses);
  }
  code.push_back(EncodeSop1(0x04, 126, 193));
  for (u32 i = 0; i < 4; ++i)
    AppendStoreVgprAtLaneDwordOffset(&code, 24 + i, 2, OutputStart + LaneCount * 4 + i);
  AppendEnd(&code);
  // Bound the first mapping to one guest page. Its default size would extend
  // into the next page and overlap the deliberately noncontiguous mapping.
  test.bda_mappings = {{GuestBase, NodeStart * sizeof(u32), 0x4000},
                       {GuestBase + 0x4000, (NodeStart + 257 * 16) * sizeof(u32), 64}};
  test.compute_info.wave_size = wave;
  test.compute_info.threads_num[0] = LaneCount;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  test.required_spirv = {"get_bda_pointer"};
  return test;
}
