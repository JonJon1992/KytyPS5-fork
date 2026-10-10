// Diagnostic capture checks, included after the GPU fixtures and RunCase.
namespace BvhCaptureTests {
namespace Capture = ShaderRecompiler::BvhCapture;

void CheckFixture(VulkanHarness& vulkan, bool node64, bool a16) {
  auto test = BvhIntersections(false, true, 0, node64, a16);
  const auto normalized = BvhIntersections(false, true).initial;
  const uint64_t bias = node64 ? 0x800010000ull >> 3 : 0;
  test.shader_hash = 1;
  test.bvh_capture_capacity = 64;
  test.check_bvh_capture = [&](std::span<const u32> words) {
    const auto count = words[Capture::Count];
    Require(test.name, "GPU witness", count == 52 && count <= test.bvh_capture_capacity,
            "invalid captured record count");
    bool crossing = false;
    std::array<bool, 4> triangles{};
    std::set<std::pair<u32, u32>> calls;
    const u32 components = (a16 ? 8u : 11u) + (node64 ? 1u : 0u);
    const u32 nsa = (components - 1 + 3) / 4;
    for (u32 i = 0; i < count; ++i) {
      const auto record = words.subspan(Capture::HeaderWords + i * Capture::RecordWords,
                                        Capture::RecordWords);
      const auto lane = record[Capture::Invocation];
      Require(test.name, "invocation identity", lane < 32 &&
                  record[Capture::Invocation + 1] == 0 && record[Capture::Invocation + 2] == 0,
              "invalid invocation in capture");
      const uint64_t node = record[Capture::NodeLow] | (uint64_t(record[Capture::NodeHigh]) << 32);
      const uint64_t input_node = test.initial[lane * 16] |
          (node64 ? uint64_t(test.initial[lane * 16 + 1]) << 32 : 0);
      Require(test.name, "node identity", node == input_node, "captured a different node");
      Require(test.name, "normalized ray", std::equal(record.begin() + Capture::Extent,
              record.begin() + Capture::Extent + 10, normalized.begin() + lane * 16 + 1),
              "captured ray differs from normalized GPU input");
      const u32 kind = node & 7;
      const u32 size = record[Capture::NodeWords];
      const uint64_t index = (node - bias) >> 3;
      Require(test.name, "node size", size == 0 || size == (kind == 5 ? 32u : 16u), "wrong BVH block count");
      for (u32 j = 0; j < size; ++j) {
        const auto input = 1024 + index * 16 + j + (index == 255 && j >= 16 ? 16 : 0);
        Require(test.name, "GPU node bytes", record[Capture::Data + j] == test.initial.at(input),
                "node bytes did not follow the guest page table");
      }
      u32 call = 0;
      bool found = false;
      for (u32 j = 0; j < test.code.size(); ++j) {
        if (test.code[j] != EncodeMimg0(node64 ? 0xe7 : 0xe6, 15, 0, false, 0, true) &&
            test.code[j] != EncodeMimg0(node64 ? 0xe7 : 0xe6, 15, nsa, false, 0, true)) continue;
        if (record[Capture::Pc] == j * 4) { found = true; break; }
        ++call;
      }
      Require(test.name, "guest PC", found && call < 2, "unknown BVH instruction PC");
      Require(test.name, "single execution", calls.emplace(call, lane).second,
              "duplicate witness for a single execution");
      Require(test.name, "EXEC capture", call == 0 || lane < 20, "inactive invocation captured");
      const bool rejected = (lane >= 8 && lane < 12) || lane % 8 == 7 || kind > 5 ||
                            (node64 && lane >= 30) || (call == 1 && kind == 5);
      Require(test.name, "guard status", record[Capture::Status] ==
                  (rejected ? Capture::InvalidInput : Capture::Resident) &&
                  (size == 0) == rejected,
              "capture omitted or accessed a rejected input");
      Require(test.name, "software reference", std::equal(record.begin() + Capture::Result,
              record.begin() + Capture::Result + 4, test.expected.begin() + 512 + call * 128 + lane * 4),
              "capture changed or misreported the software result");
      if (kind < 4 && size) triangles[kind] = true;
      crossing |= index == 255;
    }
    Require(test.name, "leaf kinds", std::ranges::all_of(triangles, [](bool v) { return v; }),
            "capture omitted a triangle kind");
    Require(test.name, "cross-page node", crossing, "capture omitted the noncontiguous FP32 node");
  };
  RunCase(&vulkan, test);
  if (node64 || a16) return;
  test.bvh_capture_enabled = false;
  test.check_bvh_capture = [&](std::span<const u32> words) {
    Require(test.name, "disabled capture", words[Capture::Count] == 0,
            "disabled capture recorded a node");
  };
  RunCase(&vulkan, test);
  test.bvh_capture_enabled = true;
  test.bvh_capture_capacity = 1;
  test.check_bvh_capture = [](std::span<const u32> words) {
    Require("BvhCapture", "bounded storage", words[Capture::Count] == 1 &&
                words[Capture::HeaderWords + Capture::Status] != 0,
            "capture did not saturate exactly at its capacity");
  };
  RunCase(&vulkan, test);
  // A frequent box instruction must not consume the slots requested for a
  // different instruction. The shader output remains checked by RunCase.
  std::vector<u32> pcs;
  for (u32 i = 0; i < test.code.size(); ++i) {
    if (test.code[i] == EncodeMimg0(0xe6, 15, 0, false, 0, true) ||
        test.code[i] == EncodeMimg0(0xe6, 15, 3, false, 0, true)) pcs.push_back(i * 4);
  }
  Require("BvhCapturePcFilter", "two instructions", pcs.size() == 2,
          "fixture must contain its two BVH instructions");
  test.name = "BvhCapturePcFilter";
  test.bvh_capture_capacity = 64;
  for (const auto [selected, expected] :
       std::array<std::pair<u32, u32>, 3>{{{pcs[0], 32}, {pcs[1], 20}, {0xfffffffcu, 0}}}) {
    test.bvh_capture_pc_filter = selected;
    test.check_bvh_capture = [selected, expected](std::span<const u32> words) {
      Require("BvhCapturePcFilter", "selected instruction count", words[Capture::Count] == expected,
              "capture ignored the PC filter or lost matching active invocations");
      for (u32 i = 0; i < words[Capture::Count]; ++i) {
        Require("BvhCapturePcFilter", "selected instruction identity",
                words[Capture::HeaderWords + i * Capture::RecordWords + Capture::Pc] == selected,
                "capture included an unselected instruction");
      }
    };
    RunCase(&vulkan, test);
  }
}

void CheckInvocations(VulkanHarness& vulkan, u32 wave, bool two_dimensional, bool unmapped = false,
                      bool barycentrics = false,
                      const std::function<void(std::span<const u32>)>& observer = {},
                      u32 sample_mask = 0) {
  TestCase test;
  test.name = two_dimensional ? "BvhCaptureInvocationXY" : "BvhCaptureInvocation64";
  if (unmapped) test.name = "BvhCaptureUnmapped";
  test.shader_hash = 1;
  test.bvh_capture_capacity = 64;
  test.bvh_capture_sample_mask = sample_mask;
  test.initial.resize(1024 + 16);
  constexpr std::array<float, 15> vertices{-1,-1,2, 1,-1,2, 0,1,2, 2,1,2, 0,3,2};
  for (u32 i = 0; i < vertices.size(); ++i) test.initial[1024 + i] = std::bit_cast<u32>(vertices[i]);
  test.initial[1024 + 15] = 0x04040404;
  test.expected = test.initial;
  const std::array<u32, 4> result = unmapped ? std::array<u32, 4>{0x7f800000, 0x3f800000, ~0u, 0}
      : std::array<u32, 4>{std::bit_cast<u32>(-8.f), std::bit_cast<u32>(-4.f),
                          barycentrics ? std::bit_cast<u32>(-1.f) : 0x04040404,
                          barycentrics ? std::bit_cast<u32>(-1.f) : 1};
  for (u32 i = 0; i < 64; ++i) std::copy(result.begin(), result.end(), test.expected.begin() + i * 4);
  auto& code = test.code;
  AppendSMovLiteral(&code, 8, 0x100);
  AppendSMovLiteral(&code, 9, 0x80000000);
  AppendSMovLiteral(&code, 10, 0);
  AppendSMovLiteral(&code, 11, barycentrics ? 0x81000000 : 0x80000000);
  if (two_dimensional) {
    code.push_back(EncodeVop2(0x1a, 2, InlineU32(3), 1)); // linear = local_y * 8 + local_x
    code.push_back(EncodeVop2(0x25, 2, Vgpr(0), 2));
  } else {
    code.push_back(EncodeVop1(0x01, 2, Vgpr(0)));
  }
  code.push_back(EncodeVop2(0x1a, 2, InlineU32(2), 2));
  constexpr std::array<u32, 11> ray{0, 0x41200000, 0,0,0, 0,0,0x3f800000,
                                   0x7f800000,0x7f800000,0x3f800000};
  for (u32 i = 0; i < ray.size(); ++i) AppendVMovLiteral(&code, 4 + i, ray[i]);
  const auto pc = code.size() * 4;
  code.insert(code.end(), {EncodeMimg0(0xe6, 15, 0, false, 0, true), EncodeMimg1(20, 4, 2)});
  for (u32 i = 0; i < 4; ++i) AppendStoreVgprAtLaneDwordOffset(&code, 20 + i, 2, i);
  AppendEnd(&code);
  test.bda_mappings = {{unmapped ? 0x20000ull : 0x10000ull, 1024 * 4, 64}};
  test.compute_info.wave_size = wave;
  test.compute_info.threads_num[0] = two_dimensional ? 8 : 64;
  test.compute_info.threads_num[1] = two_dimensional ? 8 : 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = two_dimensional ? 2 : 1;
  test.has_compute_info = true;
  test.check_bvh_capture = [&](std::span<const u32> words) {
    Require(test.name, "invocations", words[Capture::Count] == 64 / (sample_mask + 1),
            "capture lost a wave half or ignored its sampling mask");
    std::array<bool, 64> seen{};
    for (u32 i = 0; i < words[Capture::Count]; ++i) {
      auto record = words.subspan(Capture::HeaderWords + i * Capture::RecordWords, Capture::RecordWords);
      const auto x = record[Capture::Invocation], y = record[Capture::Invocation + 1];
      Require(test.name, "grid", x < test.compute_info.threads_num[0] &&
                  y < test.compute_info.threads_num[1] && record[Capture::Invocation + 2] == 0,
              "invocation lies outside the guest workgroup");
      const auto index = x + y * test.compute_info.threads_num[0];
      Require(test.name, "sampling", ((x ^ (y * 0x9e3779b9u)) & sample_mask) == 0,
              "capture recorded an invocation excluded by sampling");
      Require(test.name, "unique invocation", !seen[index], "wave halves share an invocation ID");
      seen[index] = true;
      Require(test.name, "PC/result", record[Capture::Pc] == pc &&
                  std::equal(result.begin(), result.end(), record.begin() + Capture::Result),
              "wrong instruction or result in the witness");
      Require(test.name, "mapping guard", record[Capture::Status] ==
                  (unmapped ? Capture::Unmapped : Capture::Resident) &&
                  record[Capture::NodeWords] == (unmapped ? 0u : 16u),
              "capture accessed unmapped node storage");
    }
    if (observer) observer(words);
  };
  RunCase(&vulkan, test);
}

class Environment {
public:
  Environment(const char* name, const char* value) : m_name(name) {
    const auto* saved = std::getenv(name);
    if (saved) m_saved = saved;
    SetEnvironment(name, value);
  }
  ~Environment() { SetEnvironment(m_name, m_saved ? m_saved->c_str() : nullptr); }
private:
  const char* m_name;
  std::optional<std::string> m_saved;
};

void CheckRuntime(VulkanHarness& vulkan) {
  constexpr const char* name = "BvhCaptureRuntime";
  const auto root = std::filesystem::temp_directory_path() /
      ("kyty-bvh-capture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(root);
  Environment skip("KYTY_BVH_CAPTURE_SKIP", "1");
  Environment pc_filter("KYTY_BVH_CAPTURE_PC", "2748"); // 0xabc, the fixture's selected PC.
  for (bool indirect : {false, true}) {
    const auto folder = root / (indirect ? "indirect" : "direct");
    const auto trigger = root / (indirect ? "indirect.trigger" : "direct.trigger");
    Environment directory("KYTY_BVH_CAPTURE_DIR", folder.string().c_str());
    Environment gated("KYTY_BVH_CAPTURE_TRIGGER", trigger.string().c_str());
    auto renderer = vulkan.MakeRenderContext();
    auto& scheduler = renderer->GetCommandScheduler();
    HW::Context registers;
    HW::UserConfig user;
    HW::Shader shaders;
    scheduler.Begin(registers, user, shaders);
    auto& cache = renderer->GetBufferCache();
    Require(name, "shader filter", !cache.BeginBvhCapture(2, 3, 4, 5, 17), "wrong shader armed capture");
    Require(name, "trigger gate", !cache.BeginBvhCapture(1, 3, 4, 5, 17), "missing trigger armed capture");
    std::ofstream(trigger).close();
    Require(name, "skip", !cache.BeginBvhCapture(1, 3, 4, 5, 17), "skip budget ignored");
    std::unique_ptr<Libs::Graphics::Buffer> args;
    if (indirect) {
      args = std::make_unique<Libs::Graphics::Buffer>(vulkan.RuntimeContext(), scheduler,
                  MemoryUsage::DeviceLocal, 0, AllFlags, 32);
      const std::array<u32, 3> groups{3, 4, 5};
      scheduler.Current().Handle().updateBuffer(args->Handle(), 4, sizeof(groups), groups.data());
    }
    Require(name, "arm", cache.BeginBvhCapture(1, indirect ? 0 : 3, indirect ? 0 : 4,
                indirect ? 0 : 5, 17, args.get(), 4), "matching dispatch was not armed");
    // This fixture tests the production transfer/completion path. The guest
    // recompiler's actual records are tested independently above.
    auto* fault = cache.GetFaultBuffer();
    fault->Fill(BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE + Capture::Count * 4, 4, 1);
    fault->Fill(BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE +
                   (Capture::HeaderWords + Capture::Pc) * 4, 4, 0xabc);
    cache.EndBvhCapture(1);
    Require(name, "single dispatch", !cache.BeginBvhCapture(1, 3, 4, 5, 17), "capture rearmed");
    if (!indirect) scheduler.Finish();
    renderer.reset(); // Indirect case exercises shutdown with the readback still pending.
    args.reset();
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) files.push_back(entry.path());
    Require(name, "one file", files.size() == 1, "capture did not publish exactly one file");
    Require(name, "file size", std::filesystem::file_size(files[0]) ==
                (Capture::HeaderWords + Capture::RecordWords) * 4, "unbounded or truncated file");
    std::array<u32, Capture::HeaderWords + Capture::RecordWords> words{};
    std::ifstream file(files[0], std::ios::binary);
    file.read(reinterpret_cast<char*>(words.data()), sizeof(words));
    Require(name, "readback", bool(file) && words[Capture::Count] == 1 &&
                words[Capture::FileMagic] == Capture::Magic && words[Capture::FileVersion] == Capture::Version &&
                words[Capture::Stride] == Capture::RecordWords && words[Capture::ShaderLow] == 1 &&
                words[Capture::PcFilter] == 0xabc &&
                words[Capture::GroupsX] == 3 && words[Capture::GroupsY] == 4 && words[Capture::GroupsZ] == 5 &&
                words[Capture::Mode] == 17 && words[Capture::Indirect] == u32(indirect) &&
                words[Capture::HeaderWords + Capture::Pc] == 0xabc,
            "file does not match GPU contents or dispatch arguments");
  }
  std::filesystem::remove_all(root);
  std::puts("PASS BvhCaptureRuntime: trigger, skip, direct/indirect GPU dimensions, one file and pending shutdown");
}

void CheckEmptyRetry(VulkanHarness& vulkan) {
  constexpr const char* name = "BvhCaptureEmptyRetry";
  const auto root = std::filesystem::temp_directory_path() /
      ("kyty-bvh-empty-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Environment directory("KYTY_BVH_CAPTURE_DIR", root.string().c_str());
  Environment gated("KYTY_BVH_CAPTURE_TRIGGER", nullptr);
  Environment skip("KYTY_BVH_CAPTURE_SKIP", "1");
  auto renderer = vulkan.MakeRenderContext();
  auto& scheduler = renderer->GetCommandScheduler();
  HW::Context registers;
  HW::UserConfig user;
  HW::Shader shaders;
  scheduler.Begin(registers, user, shaders);
  auto& cache = renderer->GetBufferCache();
  Require(name, "skip", !cache.BeginBvhCapture(1, 1, 1, 1, 0), "skip budget ignored");
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    Require(name, "arm", cache.BeginBvhCapture(1, 1, 1, 1, 0), "empty capture was not retried");
    cache.EndBvhCapture(1);
    Require(name, "pending gate", !cache.BeginBvhCapture(1, 1, 1, 1, 0), "rearmed before completion");
    scheduler.Finish();
  }
  Require(name, "retry bound", !cache.BeginBvhCapture(1, 1, 1, 1, 0), "empty retries are unbounded");
  renderer.reset();
  unsigned files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    Require(name, "empty size", std::filesystem::file_size(entry.path()) == Capture::HeaderWords * 4,
            "empty capture exposed stale records");
    ++files;
  }
  Require(name, "bounded files", files == 16, "empty captures overwrote a file");
  std::filesystem::remove_all(root);
  std::puts("PASS BvhCaptureEmptyRetry: completion gate and 16-attempt bound");
}

void CheckRetryStopsAfterRecords(VulkanHarness& vulkan) {
  constexpr const char* name = "BvhCaptureRetryStops";
  const auto root = std::filesystem::temp_directory_path() /
      ("kyty-bvh-retry-stops-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Environment directory("KYTY_BVH_CAPTURE_DIR", root.string().c_str());
  Environment gated("KYTY_BVH_CAPTURE_TRIGGER", nullptr);
  Environment skip("KYTY_BVH_CAPTURE_SKIP", "1");
  auto renderer = vulkan.MakeRenderContext();
  auto& scheduler = renderer->GetCommandScheduler();
  HW::Context registers;
  HW::UserConfig user;
  HW::Shader shaders;
  scheduler.Begin(registers, user, shaders);
  auto& cache = renderer->GetBufferCache();
  Require(name, "skip", !cache.BeginBvhCapture(1, 1, 1, 1, 0), "skip budget ignored");
  Require(name, "first arm", cache.BeginBvhCapture(1, 1, 1, 1, 0), "first capture was not armed");
  cache.EndBvhCapture(1);
  scheduler.Finish();
  Require(name, "retry", cache.BeginBvhCapture(1, 1, 1, 1, 0), "empty capture was not retried");
  cache.GetFaultBuffer()->Fill(BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE + Capture::Count * 4, 4, 1);
  cache.EndBvhCapture(1);
  Require(name, "pending gate", !cache.BeginBvhCapture(1, 1, 1, 1, 0), "rearmed before completion");
  scheduler.Finish();
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    Require(name, "nonempty stop", !cache.BeginBvhCapture(1, 1, 1, 1, 0), "nonempty capture was retried");
    scheduler.Finish();
  }
  renderer.reset();
  unsigned empty = 0, nonempty = 0;
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    const auto size = std::filesystem::file_size(entry.path());
    empty += size == Capture::HeaderWords * 4;
    nonempty += size == (Capture::HeaderWords + Capture::RecordWords) * 4;
  }
  Require(name, "files", empty == 1 && nonempty == 1, "retry did not stop after the first nonempty capture");
  std::filesystem::remove_all(root);
  std::puts("PASS BvhCaptureRetryStops: empty completion, nonempty retry and permanent stop");
}

void CheckAllShadersRotation(VulkanHarness& vulkan) {
  constexpr const char* name = "BvhCaptureAllShaders";
  auto options = ShaderRecompiler::GetCodegenOptions();
  options.bvh_capture_shader = UINT64_MAX;
  CodegenTests::ScopedCodegenOptions capture_options(options);
  const auto root = std::filesystem::temp_directory_path() /
      ("kyty-bvh-all-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Environment directory("KYTY_BVH_CAPTURE_DIR", root.string().c_str());
  Environment gated("KYTY_BVH_CAPTURE_TRIGGER", nullptr);
  Environment skip("KYTY_BVH_CAPTURE_SKIP", "1");
  auto renderer = vulkan.MakeRenderContext();
  auto& scheduler = renderer->GetCommandScheduler();
  HW::Context registers;
  HW::UserConfig user;
  HW::Shader shaders;
  scheduler.Begin(registers, user, shaders);
  auto& cache = renderer->GetBufferCache();
  Require(name, "skip", !cache.BeginBvhCapture(111, 1, 1, 1, 0), "skip budget ignored");
  for (u32 hash : {111u, 222u, 333u}) {
    if (hash > 111)
      Require(name, "empty shader excluded", !cache.BeginBvhCapture(111, 1, 1, 1, 0),
              "an empty first shader starved later shaders");
    if (hash > 222)
      Require(name, "second empty excluded", !cache.BeginBvhCapture(222, 1, 1, 1, 0),
              "an empty second shader starved later shaders");
    Require(name, "next shader", cache.BeginBvhCapture(hash, 1, 1, 1, 0), "later shader was not captured");
    if (hash == 333)
      cache.GetFaultBuffer()->Fill(BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE + Capture::Count * 4, 4, 1);
    cache.EndBvhCapture(hash);
    Require(name, "pending gate", !cache.BeginBvhCapture(hash + 1, 1, 1, 1, 0),
            "another shader armed before completion");
    scheduler.Finish();
  }
  Require(name, "nonempty stop", !cache.BeginBvhCapture(444, 1, 1, 1, 0), "capture continued after records");
  renderer.reset();
  unsigned files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(root)) ++files;
  Require(name, "one attempt per shader", files == 3, "shader rotation produced unexpected files");
  std::filesystem::remove_all(root);
  std::puts("PASS BvhCaptureAllShaders: skip completed empty shaders and stop on records");
}

void CheckAstroScene(VulkanHarness& vulkan, bool missing) {
  const auto saved=ShaderRecompiler::GetCodegenOptions();
  auto options=saved;options.bvh_capture_shader=Capture::AstroShader;
  ShaderRecompiler::SetCodegenOptions(options);
  TestCase test;
  test.name = missing ? "AstroSceneMissingPage" : "AstroSceneCrossPage";
  test.shader_hash = Capture::AstroShader;
  test.initial.resize(0x7000/4);
  test.bvh_capture_capacity = 32;
  constexpr u32 base=0x13f00, instance=0x20000;
  std::array<u32,232> blas{};
  for(u32 i=0;i<blas.size();++i)blas[i]=0xa1000000+i;
  blas[0]=0x5f525350;blas[1]=0x4c485642;
  blas[4]=928;blas[5]=0;blas[8]=768;blas[9]=0;
  blas[20]=12;blas[21]=0;blas[22]=116;blas[23]=0;
  constexpr std::array<float,15> vertices{-1,-1,2, 1,-1,2, 0,1,2, 2,1,2, 0,3,2};
  for(u32 i=0;i<vertices.size();++i)blas[32+i]=std::bit_cast<u32>(vertices[i]);
  blas[47]=0x04040404;
  for(u32 i=0;i<blas.size();++i)test.initial[(i<64?0x4000/4:0x6000/4-64)+i]=blas[i];
  auto inst=std::span(test.initial).subspan(0x5000/4,40);
  inst[0]=0x104;inst[2]=base;inst[4]=inst[9]=inst[14]=0x3f800000;inst[38]=37;
  test.expected=test.initial;
  constexpr std::array<u32,4> output{0xc1000000,0xc0800000,0xbf800000,0xbf800000};
  for(u32 i=0;i<32;++i)std::copy(output.begin(),output.end(),test.expected.begin()+i*4);
  auto& code=test.code;
  AppendSMovLiteral(&code,8,base>>8);AppendSMovLiteral(&code,9,0x80000000);
  AppendSMovLiteral(&code,10,11);AppendSMovLiteral(&code,11,0x81000000);
  AppendSMovLiteral(&code,46,instance);AppendSMovLiteral(&code,47,0);
  AppendSMovLiteral(&code,35,0x80000049);AppendSMovLiteral(&code,19,4);
  code.push_back(EncodeVop2(0x1a,2,InlineU32(2),0));
  constexpr std::array<u32,11> ray{16,0x41200000,0,0,0,0,0,0x3f800000,
                                  0x7f800000,0x7f800000,0x3f800000};
  for(u32 i=0;i<ray.size();++i)AppendVMovLiteral(&code,4+i,ray[i]);
  while(code.size()*4<0x520)code.push_back(EncodeSopp(0));
  code.insert(code.end(),{EncodeMimg0(0xe6,15,0,false,0,true),EncodeMimg1(20,4,2)});
  for(u32 i=0;i<4;++i)AppendStoreVgprAtLaneDwordOffset(&code,20+i,2,i);
  AppendEnd(&code);
  test.bda_mappings={{base,0x4000,256},{instance,0x5000,160}};
  if(!missing)test.bda_mappings.push_back({0x14000,0x6000,672});
  test.compute_info.wave_size=32;test.compute_info.threads_num[0]=32;
  test.compute_info.threads_num[1]=test.compute_info.threads_num[2]=1;
  test.compute_info.thread_ids_num=1;test.has_compute_info=true;
  test.check_bvh_capture=[&](std::span<const u32> words) {
    Require(test.name,"record count",words[Capture::Count]==32,"lost BVH records");
  };
  test.check_bvh_scenes=[&](std::span<const u32> words) {
    for(u32 i=0;i<32;++i) {
      auto scene=words.subspan(i*Capture::SceneWords,Capture::SceneWords);
      Require(test.name,"coherent scene status",scene[Capture::SceneStatus]==
              (missing?Capture::MissingScene:Capture::CompleteScene),"scene was not captured or incomplete scene accepted");
      Require(test.name,"scene identity",scene[Capture::BlasLow]==base &&
              scene[Capture::BlasBytes]==928 && scene[Capture::SceneInstanceLow]==instance &&
              scene[Capture::SceneLeafId]==0x80000049 && scene[Capture::SceneFlags]==4,
              "lost BLAS, instance or acceptance context");
      if(!missing)Require(test.name,"complete noncontiguous bytes",
              std::equal(blas.begin(),blas.end(),scene.begin()+Capture::BlasData) &&
              std::equal(inst.begin(),inst.end(),scene.begin()+Capture::InstanceData),
              "GPU scene copy did not follow each mapped page");
    }
  };
  RunCase(&vulkan,test);
  ShaderRecompiler::SetCodegenOptions(saved);
}

void Run(VulkanHarness& vulkan) {
  CheckAstroScene(vulkan,false);
  CheckAstroScene(vulkan,true);
  for (bool node64 : {false, true}) for (bool a16 : {false, true}) CheckFixture(vulkan, node64, a16);
  for (u32 wave : {32u, 64u}) for (bool xy : {false, true}) CheckInvocations(vulkan, wave, xy);
  CheckInvocations(vulkan, 64, true, true);
  CheckInvocations(vulkan, 32, false, false, false, {}, 3);
  CheckInvocations(vulkan, 64, true, false, false, {}, 3);
  CheckRuntime(vulkan);
  CheckEmptyRetry(vulkan);
  CheckRetryStopsAfterRecords(vulkan);
  CheckAllShadersRotation(vulkan);
}
} // namespace BvhCaptureTests
