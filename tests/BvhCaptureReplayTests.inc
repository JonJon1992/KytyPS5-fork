// Offline hardware audit of individual leaves captured by a real guest shader.
// This does not replace shader traversal or reproduce its acceptance policy.
namespace BvhCaptureReplay {
namespace Capture = ShaderRecompiler::BvhCapture;
using Record = std::array<u32, Capture::RecordWords>;
struct Witness {
  std::array<u32, Capture::HeaderWords> header{};
  std::vector<Record> records;
};

std::optional<Witness> Read(const std::filesystem::path& path, std::string& error) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  const auto size = file.tellg();
  if (!file || size < std::streamoff(Capture::HeaderWords * 4) || size > std::streamoff(Capture::Bytes)) {
    error = "file size is outside the bounded witness format";
    return {};
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
    error = "incomplete witness file";
    return {};
  }
  const auto word = [&](size_t i) {
    const auto at = i * 4;
    return u32(bytes[at]) | (u32(bytes[at+1]) << 8) | (u32(bytes[at+2]) << 16) | (u32(bytes[at+3]) << 24);
  };
  Witness witness;
  auto& h = witness.header;
  for (u32 i = 0; i < Capture::HeaderWords; ++i) h[i] = word(i);
  if (h[Capture::FileMagic] != Capture::Magic ||
      (h[Capture::FileVersion] != 1 && h[Capture::FileVersion] != Capture::Version) ||
      h[Capture::Stride] != Capture::RecordWords || h[Capture::Enabled] != 1 ||
      h[Capture::Capacity] == 0 || h[Capture::Capacity] > Capture::MaxRecords ||
      h[Capture::Count] > h[Capture::Capacity] || h[Capture::Indirect] > 1 ||
      bytes.size() != (Capture::HeaderWords + uint64_t(h[Capture::Count]) * Capture::RecordWords) * 4) {
    error = "invalid witness header or record count";
    return {};
  }
  witness.records.resize(h[Capture::Count]);
  for (u32 i = 0; i < witness.records.size(); ++i) {
    auto& r = witness.records[i];
    for (u32 j = 0; j < Capture::RecordWords; ++j) r[j] = word(Capture::HeaderWords + i * Capture::RecordWords + j);
    if (h[Capture::FileVersion] == 1) r[Capture::Status] = Capture::Resident;
    const auto status = r[Capture::Status], kind = r[Capture::NodeLow] & 7;
    if (status < Capture::Resident || status > Capture::Unmapped ||
        (status == Capture::Resident && (kind > 5 || r[Capture::NodeWords] != (kind == 5 ? 32 : 16))) ||
        (status != Capture::Resident && r[Capture::NodeWords] != 0)) {
      error = "invalid node status or payload size";
      return {};
    }
  }
  return witness;
}

void Write(const std::filesystem::path& path, const Witness& witness) {
  std::ofstream file(path, std::ios::binary);
  const auto write = [&](std::span<const u32> words) {
    for (u32 word : words) {
      const std::array<char, 4> bytes{char(word), char(word >> 8), char(word >> 16), char(word >> 24)};
      file.write(bytes.data(), bytes.size());
    }
  };
  write(witness.header);
  for (const auto& record : witness.records) write(record);
  Require("BvhCaptureReader", "write fixture", bool(file), "failed to write the witness fixture");
}

void CheckReader() {
  constexpr const char* name = "BvhCaptureReader";
  const auto root = std::filesystem::temp_directory_path() /
      ("kyty-bvh-reader-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(root);
  const auto path = root / "witness.bin";
  Witness w;
  w.header[Capture::Count] = 1;
  w.header[Capture::Enabled] = 1;
  w.header[Capture::FileMagic] = Capture::Magic;
  w.header[Capture::FileVersion] = Capture::Version;
  w.header[Capture::Stride] = Capture::RecordWords;
  w.header[Capture::Capacity] = 64;
  w.records.resize(1);
  w.records[0][Capture::NodeWords] = 16;
  w.records[0][Capture::Status] = Capture::Resident;
  std::string error;
  Write(path, w);
  Require(name, "valid v2", bool(Read(path, error)), "valid witness rejected");
  w.header[Capture::FileVersion] = 1;
  w.records[0][Capture::Status] = 0;
  Write(path, w);
  Require(name, "legacy v1", bool(Read(path, error)), "valid old witness rejected");
  w.header[Capture::FileVersion] = Capture::Version;
  w.records[0][Capture::Status] = Capture::Unmapped;
  w.records[0][Capture::NodeWords] = 0;
  Write(path, w);
  Require(name, "unmapped", bool(Read(path, error)), "valid unmapped witness rejected");
  const auto reject = [&](const char* field, const Witness& bad) {
    Write(path, bad);
    Require(name, field, !Read(path, error), std::string("accepted invalid ") + field);
  };
  auto bad = w; bad.header[Capture::Count] = 2; reject("count", bad);
  bad = w; bad.header[Capture::Capacity] = Capture::MaxRecords + 1; reject("capacity", bad);
  bad = w; bad.header[Capture::FileVersion] = 999; reject("version", bad);
  bad = w; bad.records[0][Capture::NodeWords] = 16; reject("unmapped payload", bad);
  bad = w; bad.records[0][Capture::Status] = 999; reject("status", bad);
  bad = w; bad.records[0][Capture::Status] = Capture::Resident;
  bad.records[0][Capture::NodeWords] = 16; bad.records[0][Capture::NodeLow] = 7; reject("kind", bad);
  Write(path, w);
  std::filesystem::resize_file(path, 255);
  Require(name, "truncated header", !Read(path, error), "accepted incomplete header");
  Write(path, w);
  std::filesystem::resize_file(path, Capture::HeaderWords * 4 + 255);
  Require(name, "truncated record", !Read(path, error), "accepted incomplete record");
  std::filesystem::remove_all(root);
  std::puts("PASS BvhCaptureReader: v1/v2, bounds, status and truncation");
}

struct Statistics {
  uint64_t queries = 0, skipped_guards = 0, skipped_boxes = 0, skipped_rays = 0, skipped_geometry = 0;
  uint64_t hit_mismatches = 0, distance_mismatches = 0, barycentric_mismatches = 0;
};

Statistics Compare(VulkanHarness& vulkan, const Witness& witness, std::ostream& csv) {
  constexpr const char* name = "HardwareRtWitness";
  struct Entry { u32 record; RT::Ray ray; };
  using GeometryKey = std::array<u32, 17>;
  std::map<GeometryKey, std::vector<Entry>> groups;
  Statistics stats;
  for (u32 i = 0; i < witness.records.size(); ++i) {
    const auto& r = witness.records[i];
    if (r[Capture::Status] != Capture::Resident) { ++stats.skipped_guards; continue; }
    if ((r[Capture::NodeLow] & 7) >= 4) { ++stats.skipped_boxes; continue; }
    RT::Ray ray{};
    ray.max_t = std::numeric_limits<float>::max();
    bool finite = true;
    for (u32 axis = 0; axis < 3; ++axis) {
      ray.origin[axis] = std::bit_cast<float>(r[Capture::Origin + axis]);
      ray.direction[axis] = std::bit_cast<float>(r[Capture::Direction + axis]);
      finite &= std::isfinite(ray.origin[axis]) && std::isfinite(ray.direction[axis]);
    }
    if (!finite || (ray.direction[0] == 0 && ray.direction[1] == 0 && ray.direction[2] == 0)) {
      ++stats.skipped_rays; continue;
    }
    GeometryKey key;
    std::copy_n(r.begin() + Capture::Data, 16, key.begin());
    key[16] = r[Capture::NodeLow] & 7;
    groups[key].push_back({i, ray});
  }
  auto renderer = vulkan.MakeRenderContext();
  auto& backend = renderer->GetHardwareRt();
  Require(name, "features", backend.Enabled(), "hardware RT is unavailable");
  auto& scheduler = renderer->GetCommandScheduler();
  HW::Context registers;
  HW::UserConfig user;
  HW::Shader shaders;
  scheduler.Begin(registers, user, shaders);
  struct Pending {
    const GeometryKey* geometry;
    const std::vector<Entry>* entries;
    RT::HardwareBackend::QueryLease query;
  };
  std::vector<Pending> pending;
  csv << "record,pc,node_low,node_high,invocation_x,invocation_y,invocation_z,software_hit,hardware_hit,"
         "software_t,hardware_t,hardware_u,hardware_v,hit_match,distance_match,barycentric_match\n";
  csv << std::setprecision(9);
  const auto close = [](float a, float b, float tolerance) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a-b) <= tolerance * std::max(1.f, std::abs(a));
  };
  const auto retire = [&] {
    scheduler.Finish();
    for (const auto& p : pending) {
      const auto hits = backend.Read(p.query);
      Require(name, "completion", hits.size() == p.entries->size(), "unfinished hardware query");
      for (u32 i = 0; i < hits.size(); ++i) {
        const auto& entry = (*p.entries)[i];
        const auto& r = witness.records[entry.record];
        const auto& hit = hits[i];
        const float numerator = std::bit_cast<float>(r[Capture::Result]);
        const float denominator = std::bit_cast<float>(r[Capture::Result + 1]);
        const float expected_t = numerator / denominator;
        const bool software_hit = std::isfinite(numerator) && std::isfinite(denominator) &&
                                  denominator != 0 && std::isfinite(expected_t) && expected_t >= 0;
        const bool hardware_hit = hit.primitive != ~0u;
        const bool hit_match = software_hit == hardware_hit && (!hardware_hit || hit.primitive == 0);
        const bool distance_match = !software_hit || !hardware_hit || close(expected_t, hit.t, 2e-5f);
        bool barycentric_match = true;
        if (software_hit && hardware_hit && (r[Capture::Descriptor + 3] & 0x01000000)) {
          const auto remap = (*p.geometry)[15] >> ((*p.geometry)[16] * 8);
          const std::array<float, 3> coordinates{1.f - hit.u - hit.v, hit.u, hit.v};
          const auto select = [&](u32 shift) { const auto index = (remap >> shift) & 3;
            return coordinates[index == 1 ? 1 : index == 2 ? 2 : 0]; };
          barycentric_match = close(std::bit_cast<float>(r[Capture::Result + 2]) / denominator,
                                    select(0), 1e-4f) &&
                              close(std::bit_cast<float>(r[Capture::Result + 3]) / denominator,
                                    select(2), 1e-4f);
        }
        ++stats.queries;
        stats.hit_mismatches += !hit_match;
        stats.distance_mismatches += !distance_match;
        stats.barycentric_mismatches += !barycentric_match;
        csv << entry.record << ',' << r[Capture::Pc] << ',' << r[Capture::NodeLow] << ',' << r[Capture::NodeHigh]
            << ',' << r[Capture::Invocation] << ',' << r[Capture::Invocation + 1] << ',' << r[Capture::Invocation + 2]
            << ',' << software_hit << ',' << hardware_hit << ',' << expected_t << ',' << hit.t << ','
            << hit.u << ',' << hit.v << ',' << hit_match << ',' << distance_match << ',' << barycentric_match << '\n';
      }
    }
    pending.clear();
    backend.Clear();
  };
  for (const auto& [key, entries] : groups) {
    RT::BvhSnapshot snapshot;
    // Rebase this single captured leaf, preserving all vertex and flag bits.
    // No assumption is made about missing parents, instances or shader tables.
    snapshot.address = 0x10000;
    snapshot.root = key[16];
    snapshot.descriptor = {0x100, 0, 0, 0x81000000};
    snapshot.bytes.resize(64);
    for (u32 i = 0; i < 16; ++i) for (u32 j = 0; j < 4; ++j)
      snapshot.bytes[i * 4 + j] = std::byte(key[i] >> (j * 8));
    auto scene = backend.Prepare(std::move(snapshot));
    if (!scene) { stats.skipped_geometry += entries.size(); continue; }
    std::vector<RT::Ray> rays;
    rays.reserve(entries.size());
    for (const auto& entry : entries) rays.push_back(entry.ray);
    auto query = backend.Trace(scene, rays);
    Require(name, "native query", bool(query), "valid witness rays rejected by the native backend");
    pending.push_back({&key, &entries, std::move(query)});
    if (pending.size() == 32) retire(); // Bound AS/query leases while replaying a large capture.
  }
  if (!pending.empty()) retire();
  scheduler.Finish();
  backend.Clear();
  std::printf("HardwareRtWitness: records=%zu queries=%" PRIu64
              " skipped_guard=%" PRIu64 " skipped_boxes=%" PRIu64
              " skipped_rays=%" PRIu64 " skipped_geometry=%" PRIu64 "\n",
              witness.records.size(), stats.queries, stats.skipped_guards, stats.skipped_boxes,
              stats.skipped_rays, stats.skipped_geometry);
  std::printf("HardwareRtWitness: hit_mismatches=%" PRIu64 " distance_mismatches=%" PRIu64
              " barycentric_mismatches=%" PRIu64 " (t relative tolerance 2e-5, barycentric 1e-4)\n",
              stats.hit_mismatches, stats.distance_mismatches, stats.barycentric_mismatches);
  std::puts("HardwareRtWitness: individual leaves only; guest traversal, instance/list policy and bit-exact results remain unverified");
  return stats;
}

int Run(VulkanHarness& vulkan, const char* input, const char* output) {
  std::string error;
  auto witness = Read(input, error);
  if (!witness) { std::fprintf(stderr, "HardwareRtWitness: %s\n", error.c_str()); return 2; }
  std::ofstream csv(output);
  if (!csv) { std::fprintf(stderr, "HardwareRtWitness: cannot open the CSV output\n"); return 2; }
  const auto stats = Compare(vulkan, *witness, csv);
  csv.close();
  if (!csv) { std::fprintf(stderr, "HardwareRtWitness: incomplete CSV output\n"); return 2; }
  if (stats.queries == 0) return 77; // No evidence, not a successful parity check.
  return (stats.hit_mismatches || stats.distance_mismatches || stats.barycentric_mismatches) ? 1 : 0;
}

void CheckGpuFixture(VulkanHarness& vulkan) {
  constexpr const char* name = "BvhCaptureReplayFixture";
  Witness witness;
  BvhCaptureTests::CheckInvocations(vulkan, 64, true, false, true,
      [&](std::span<const u32> words) {
        std::copy_n(words.begin(), Capture::HeaderWords, witness.header.begin());
        witness.header[Capture::FileMagic] = Capture::Magic;
        witness.header[Capture::FileVersion] = Capture::Version;
        witness.header[Capture::Stride] = Capture::RecordWords;
        witness.records.resize(words[Capture::Count]);
        for (u32 i = 0; i < witness.records.size(); ++i)
          std::copy_n(words.begin() + Capture::HeaderWords + i * Capture::RecordWords,
                      Capture::RecordWords, witness.records[i].begin());
      });
  const auto root = std::filesystem::temp_directory_path() /
      ("kyty-bvh-replay-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(root);
  const auto path = root / "gpu-witness.bin";
  Write(path, witness);
  std::string error;
  auto loaded = Read(path, error);
  Require(name, "GPU file", bool(loaded), "GPU witness rejected by bounded reader");
  std::ostringstream csv;
  auto stats = Compare(vulkan, *loaded, csv);
  Require(name, "GPU parity", stats.queries == 64 && stats.hit_mismatches == 0 &&
              stats.distance_mismatches == 0 && stats.barycentric_mismatches == 0,
          "captured software leaves disagree with the hardware query");
  const auto corrupt = [&](u32 component, float value) {
    auto bad = *loaded;
    bad.records[0][Capture::Result + component] = std::bit_cast<u32>(value);
    std::ostringstream ignored;
    return Compare(vulkan, bad, ignored);
  };
  stats = corrupt(0, std::numeric_limits<float>::infinity());
  Require(name, "detect hit mismatch", stats.hit_mismatches == 1,
          "audit accepted a corrupted hit result");
  stats = corrupt(0, -12.f);
  Require(name, "detect distance mismatch", stats.distance_mismatches == 1,
          "audit accepted a corrupted hit distance");
  stats = corrupt(2, -2.f);
  Require(name, "detect barycentric mismatch", stats.barycentric_mismatches == 1,
          "audit accepted a corrupted barycentric coordinate");
  std::filesystem::remove_all(root);
  std::puts("PASS BvhCaptureReplayFixture: GPU witness to hardware leaves and three corruption checks");
}
} // namespace BvhCaptureReplay
