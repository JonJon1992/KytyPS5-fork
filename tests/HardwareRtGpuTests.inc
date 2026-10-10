// Included inside VulkanHarness: exercise the production device, VMA buffers,
// command recorder/scheduler, push descriptors and RenderContext-owned backend.
void CheckHardwareRtBackend(bool enabled) {
  static_assert(!std::is_constructible_v<RT::HardwareBackend, RenderContext&>);
  constexpr const char* name = "HardwareRtBackend";
  auto renderer = MakeRenderContext();
  auto& backend = renderer->GetHardwareRt();
  Require(name, "capability selection", backend.Enabled() == enabled, "RT feature state differs");
  const auto fixture = [](float z) {
    RT::BvhSnapshot s;
    s.address = 0x10000;
    s.descriptor = {0x100, 0, 5, 0x81000000};
    s.root = 5;
    s.bytes.resize(6 * 64);
    auto word = [&](size_t index, u32 value) { std::memcpy(s.bytes.data() + index * 4, &value, 4); };
    auto value = [&](size_t index, float v) { word(index, std::bit_cast<u32>(v)); };
    constexpr unsigned mapping[4][3] {{0,1,2}, {1,3,2}, {2,3,4}, {2,4,0}};
    for (unsigned i = 0; i < 4; ++i) {
      word(i, ((2 + i) << 3) | i);
      for (unsigned axis = 0; axis < 6; ++axis) value(4 + i * 6 + axis, axis < 3 ? -1.f : 16.f);
      float x = float(i * 2);
      const RT::Vertex triangle[3] {{x,0,z}, {x+1,0,z}, {x,1,z}};
      for (unsigned j = 0; j < 3; ++j) {
        const auto base = (2 + i) * 16 + mapping[i][j] * 3;
        value(base, triangle[j].x); value(base+1, triangle[j].y); value(base+2, triangle[j].z);
      }
      word((2 + i) * 16 + 15, 0x04040404);
    }
    return s;
  };
  if (!enabled) {
    Require(name, "disabled fallback", !backend.Prepare(fixture(2)), "disabled backend built an AS");
    Require(name, "disabled allocations", backend.Stats().builds == 0, "unexpected build");
    std::puts("PASS HardwareRtBackend: disabled fallback");
    return;
  }
  HW::Context registers;
  HW::UserConfig user;
  HW::Shader shaders;
  auto& scheduler = renderer->GetCommandScheduler();
  scheduler.Begin(registers, user, shaders);
  std::vector<RT::Ray> rays(65); // Tests the partial final workgroup.
  for (unsigned i = 0; i < rays.size(); ++i) {
    rays[i] = {{float((i % 4) * 2) + .25f, .25f, 0}, 0, {0,0,1}, 10};
    if (i % 8 == 4) rays[i].max_t = 1;
    if (i % 8 == 5) rays[i].origin[1] = 4;
  }
  auto first = backend.Prepare(fixture(2));
  Require(name, "build", bool(first), "conversion or build rejected valid fixture");
  Require(name, "reuse", backend.Prepare(fixture(2)) == first, "identical snapshot rebuilt AS");
  auto a = backend.Trace(first, rays);
  Require(name, "query", bool(a), "valid rays rejected");
  Require(name, "readiness", backend.Read(a).empty(), "read exposed unfinished GPU output");
  auto moved = backend.Prepare(fixture(3));
  Require(name, "immutable replacement", bool(moved) && moved != first, "changed scene modified old lease");
  auto b = backend.Trace(moved, rays);
  auto flags = fixture(3);
  u32 flag = 123;
  std::memcpy(flags.bytes.data() + 47 * 4, &flag, 4);
  auto metadata = backend.Prepare(flags);
  Require(name, "metadata", metadata && backend.Primitives(metadata)[0].flags == flag,
          "metadata mapping was not refreshed");
  Require(name, "metadata reuse", backend.Stats().builds == 2, "flags rebuilt geometry");
  // Both old and new AS remain live through several queries in the same recording.
  auto c = backend.Trace(first, rays);
  // A nearest-hit-only query is wrong when the game's face rule rejects that
  // hit. The farther, oppositely wound triangle must remain a candidate.
  auto facing = fixture(2);
  auto fword = [&](size_t index, float value) {
    u32 bits=std::bit_cast<u32>(value);
    std::memcpy(facing.bytes.data()+index*4,&bits,4);
  };
  const RT::Vertex farther[3] {{0,0,4},{0,1,4},{1,0,4}};
  constexpr unsigned far_map[3] {1,3,2};
  for(unsigned j=0;j<3;j++) {
    const auto offset=3*16+far_map[j]*3;
    fword(offset,farther[j].x);fword(offset+1,farther[j].y);fword(offset+2,farther[j].z);
  }
  auto faces=backend.Prepare(facing);
  std::array<RT::Ray,2> face_rays{{{{.25f,.25f,0},0,{0,0,1},10},
                                 {{.25f,.25f,6},0,{0,0,-1},10}}};
  auto rejected_near=backend.Trace(faces,face_rays,{RT::RayPolicy::Mode::AstroFacing,0});
  auto accepted_near=backend.Trace(faces,face_rays,{RT::RayPolicy::Mode::AstroFacing,8});
  auto two_sided=backend.Trace(faces,face_rays,{RT::RayPolicy::Mode::AstroFacing,4});
  backend.Trace(metadata, rays).reset(); // Caller releases early; completion still owns it.
  backend.Clear();
  scheduler.Finish();
  const auto check = [&](const RT::HardwareBackend::QueryLease& q, float z) {
    auto hits = backend.Read(q);
    Require(name, "readback count", hits.size() == rays.size(), "query not retired");
    for (unsigned i = 0; i < hits.size(); ++i) {
      bool hit = i % 8 != 4 && i % 8 != 5;
      Require(name, "hit/primitive", hits[i].primitive == (hit ? i % 4 : ~0u), "wrong primitive/miss");
      if (hit) Require(name, "distance and barycentrics",
          std::abs(hits[i].t-z) < 1e-5f && std::abs(hits[i].u-.25f) < 1e-5f &&
          std::abs(hits[i].v-.25f) < 1e-5f, "analytic intersection mismatch");
    }
  };
  check(a, 2); check(b, 3); check(c, 2);
  for(unsigned i=0;i<2;i++) {
    const auto farther_hit=backend.Read(rejected_near)[i];
    Require(name,"rejected nearest candidate",farther_hit.primitive==(i==0?1u:0u) &&
            std::abs(farther_hit.t-4.f)<1e-5f,"face rejection hid the farther valid hit");
    Require(name,"accepted facing",backend.Read(accepted_near)[i].primitive==(i==0?0u:1u) &&
            std::abs(backend.Read(accepted_near)[i].t-2.f)<1e-5f,"wrong numerator-sign face selection");
    Require(name,"two sided",std::abs(backend.Read(two_sided)[i].t-2.f)<1e-5f,
            "two-sided rule rejected a face");
  }
  for (unsigned i = 0; i < 4; ++i)
    Require(name, "guest remapping", backend.Primitives(first)[i].node == (((2+i)<<3)|i), "lost guest pointer");
  auto invalid = rays;
  invalid[0].origin[0] = std::numeric_limits<float>::quiet_NaN();
  Require(name, "invalid ray", !backend.Trace(first, invalid), "NaN passed to RayQuery");
  auto bad = fixture(2); bad.root = 6;
  Require(name, "unsupported fallback", !backend.Prepare(bad), "instance accepted as triangle subtree");
  Require(name, "existing lease after rejection", bool(backend.Trace(first, rays)), "old valid lease invalidated");
  scheduler.Finish();
  Require(name, "new build after clear", bool(backend.Prepare(fixture(2))) && backend.Stats().builds == 4,
          "rejected snapshot reused stale entry");
  scheduler.Finish();
  backend.Clear();
  std::puts("PASS HardwareRtBackend: native AS, queries, primitive remapping, reuse, immutable replacement, retirement and fallback");
}
