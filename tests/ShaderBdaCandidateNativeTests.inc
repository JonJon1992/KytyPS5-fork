// Included in VulkanHarness: real ownership, table versions and native save/restore.
void CheckBdaWriteCandidates() {
  constexpr const char* name = "BdaWriteCandidates";
  constexpr uintptr_t base = 0x000000020f800000ull;
  constexpr uint64_t bytes = 0x100000, alignment = 0x10000;
  EnsureRuntimeContext();
  Profiler::Detail::g_event_sink.store(Profiler::Detail::CounterSink::Thread);
  auto owner = MakeRenderContext();
  auto& context = *owner;
  auto& cache = context.GetBufferCache();
  auto& scheduler = context.GetCommandScheduler();
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  context.InitializeGpu(nullptr);
  LibKernel::Memory::InstallGpuResources(&context);
  int64_t offset = -1;
  Require(name, "allocate", LibKernel::Memory::KernelAllocateDirectMemory(
      0, LibKernel::Memory::KernelGetDirectMemorySize(), bytes, alignment, 0, &offset) == 0,
      "direct allocation failed");
  void* mapped = reinterpret_cast<void*>(base);
  Require(name, "map", LibKernel::Memory::KernelMapDirectMemory(
      &mapped, bytes, 0x3, 0x10, offset, alignment) == 0 && mapped == reinterpret_cast<void*>(base),
      "direct mapping failed");
  std::memset(mapped, 0xa5, bytes);
  std::array<u32, 28> table{};
  for (u32 i = 0; i < 5; ++i) {
    const auto address = base + (i + 1) * 0x10000;
    table[8 + 4 * i] = static_cast<u32>(address);
    table[9 + 4 * i] = static_cast<u32>(address >> 32) | (4u << 16);
    table[10 + 4 * i] = 16;
    table[11 + 4 * i] = 0x5204;
  }
  std::memcpy(mapped, table.data(), sizeof(table));
  const std::array<u32, 2> user_data{static_cast<u32>(base), static_cast<u32>(base >> 32)};
  // A second table whose first destination is on the table's own 4 KiB tracker page.
  constexpr uintptr_t shared_table = base + 0x8000;
  auto shared = table;
  shared[8] = static_cast<u32>(shared_table + 0x400);
  shared[9] = static_cast<u32>((shared_table + 0x400) >> 32) | (4u << 16);
  std::memcpy(reinterpret_cast<void*>(shared_table), shared.data(), sizeof(shared));
  const std::array<u32, 2> shared_user_data{static_cast<u32>(shared_table),
                                            static_cast<u32>(shared_table >> 32)};
  BdaWriteCandidates::Plan plan;
  auto frozen = CreateHostBuffer(name, sizeof(table), vk::BufferUsageFlagBits::eTransferDst, {0});
  auto restored = CreateHostBuffer(name, sizeof(table), vk::BufferUsageFlagBits::eTransferDst, {0});
  const auto settles = Profiler::FrameEventTotal(Profiler::FrameEvent::BdaSettles);
  const auto snapshot = [&](auto& target) {
    auto& buffer = cache.GetBuffer(cache.FindBuffer(base, sizeof(table)));
    auto& command = scheduler.Current();
    command.EndRendering();
    const vk::BufferCopy copy{buffer.Offset(base), 0, sizeof(table)};
    auto native = command.Handle();
    native.copyBuffer(buffer.Handle(), target.buffer, 1, &copy);
    vk::MemoryBarrier barrier{};
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
    native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
        {}, 1, &barrier, 0, nullptr, 0, nullptr);
  };
  auto newer = table;
  newer[8] += 0x60000;
  OnGpuThread(context, [&] {
    scheduler.Begin(registers, user_config, shaders);
    context.MapMemory(base, bytes);
    const std::array<u32, 2> unaligned{user_data[0] | 1u, user_data[1]};
    BdaWriteCandidates::Plan invalid;
    Require(name, "unaligned table rejected", !cache.PrepareBdaWriteCandidates(
        ShaderRecompiler::BdaWaterLightingHash, unaligned, invalid), "unaligned table admitted");
    // Claiming that destination would make the table's page GPU-owned, so Finalize would refuse
    // after the claim: refused first, with no destination claimed.
    BdaWriteCandidates::Plan shared_plan;
    Require(name, "shared page rejected", !cache.PrepareBdaWriteCandidates(
        ShaderRecompiler::BdaWaterLightingHash, shared_user_data, shared_plan) &&
        shared_plan.reject != nullptr &&
        std::strcmp(shared_plan.reject, "destination shares a tracked page with the table") == 0,
        "a destination on the table's tracker page was admitted");
    Require(name, "shared page unclaimed",
        !cache.IsRegionGpuModified(shared_table, 0x1000) &&
        !cache.IsRegionGpuModified(base + 0x10000, 0x50000),
        "the refused plan claimed a destination");
    Require(name, "prepare", cache.PrepareBdaWriteCandidates(
        ShaderRecompiler::BdaWaterLightingHash, user_data, plan), "complete table rejected");
    Require(name, "all candidates owned", plan.range_count == 5 &&
        std::ranges::all_of(plan.Ranges(), [&](const auto& r) {
          return cache.IsRegionGpuModified(r.address, r.size);
        }), "a possible destination was left CPU-owned");
    {
      auto lease = context.PrepareHostBackingWrite(base, sizeof(table), RenderContext::HostWriter::Occlusion);
      LibKernel::Memory::WriteBacking(base, newer.data(), sizeof(newer));
    }
    SyncEpoch::Advance();
    context.PrepareBda();
    // Another guest mapping changes meanwhile (a streaming game maps and unmaps constantly): the
    // table's and destinations' own mappings are unchanged, so the proof still holds.
    {
      constexpr uintptr_t elsewhere = 0x0000000213000000ull;
      int64_t other = -1;
      void* other_mapped = reinterpret_cast<void*>(elsewhere);
      const auto generation = LibKernel::Memory::VirtualRangesGeneration();
      Require(name, "unrelated mapping",
          LibKernel::Memory::KernelAllocateDirectMemory(
              0, LibKernel::Memory::KernelGetDirectMemorySize(), alignment, alignment, 0, &other) == 0 &&
          LibKernel::Memory::KernelMapDirectMemory(&other_mapped, alignment, 0x3, 0x10, other,
                                                   alignment) == 0 &&
          LibKernel::Memory::KernelMunmap(elsewhere, alignment) == 0 &&
          LibKernel::Memory::KernelReleaseDirectMemory(other, alignment) == 0 &&
          LibKernel::Memory::VirtualRangesGeneration() != generation,
          "the unrelated guest mapping did not change the virtual ranges");
    }
    Require(name, "freeze", cache.FinalizeBdaWriteCandidates(plan), "finalization rejected");
    snapshot(frozen);
    // Native GPU writes; no known-fill shortcut for the guest readback below.
    for (u32 i = 0; i < plan.range_count; ++i) {
      const auto& range = plan.ranges[i];
      auto& buffer = cache.GetBuffer(cache.FindBuffer(range.address, range.size));
      buffer.Fill(buffer.Offset(range.address), range.size, 0xc0ffee00u + i);
    }
    // Writes the dispatch dropped, stood in for by a count left in the fault buffer: reported once
    // the recording completes, without a settle.
    const auto dropped = Profiler::FrameEventTotal(Profiler::FrameEvent::BdaCandidateDroppedWrites);
    cache.GetFaultBuffer()->Fill(BufferCache::BDA_DROPPED_WRITES_OFFSET, sizeof(u32), 7);
    cache.FinishBdaWriteCandidates(plan);
    snapshot(restored);
    Require(name, "no settle", Profiler::FrameEventTotal(Profiler::FrameEvent::BdaSettles) == settles,
        "candidate preparation submitted a synchronous settle");
    scheduler.Finish();
    Require(name, "dropped writes reported",
        Profiler::FrameEventTotal(Profiler::FrameEvent::BdaCandidateDroppedWrites) == dropped + 7,
        "a candidate dispatch's dropped writes were not reported after completion");
  });
  const auto frozen_words = ReadBuffer(name, frozen, table.size());
  const auto restored_words = ReadBuffer(name, restored, newer.size());
  Require(name, "frozen table", std::equal(table.begin(), table.end(), frozen_words.begin()),
      "GPU descriptors differ from the enumerated destinations");
  Require(name, "restored newer table", std::equal(newer.begin(), newer.end(), restored_words.begin()),
      "freezing lost the newer version uploaded before the dispatch");
  DestroyBuffer(&frozen); DestroyBuffer(&restored);
  for (u32 i = 0; i < 5; ++i) {
    const auto* words = reinterpret_cast<volatile u32*>(plan.ranges[i].address);
    for (u32 j = 0; j < 16; ++j)
      Require(name, "guest read", words[j] == 0xc0ffee00u + i, "guest did not see GPU bytes");
    Require(name, "preserved neighbour", words[16] == 0xa5a5a5a5u, "reservation lost unwritten bytes");
  }
  OnGpuThread(context, [&] {
    (void)cache.ObtainBuffer(base, sizeof(table), true);
    BdaWriteCandidates::Plan rejected;
    Require(name, "GPU table rejected", !cache.PrepareBdaWriteCandidates(
        ShaderRecompiler::BdaWaterLightingHash, user_data, rejected), "stale CPU table accepted");
    scheduler.Finish();
    context.UnmapMemory(base, bytes);
  });
  context.ShutdownGpu();
  LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name, "unmap", LibKernel::Memory::KernelMunmap(base, bytes) == 0, "unmap failed");
  Require(name, "release", LibKernel::Memory::KernelReleaseDirectMemory(offset, bytes) == 0,
      "release failed");
  owner.reset();
  std::printf("[gpu]     %-32s ok\n", name);
}
