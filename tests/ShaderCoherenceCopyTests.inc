// Included in VulkanHarness. Real BDA uploads, CP WRITE_DATA, native readback and kernel unmap.
// The worker gate makes the source-overwrite and pending-submit checks deterministic.
void CheckCoherenceCopy() {
  constexpr const char *name = "CoherenceCopy";
  constexpr uintptr_t base = 0x000000020e000000ull;
  constexpr uint64_t bytes = 0x100000;
  constexpr uint64_t alignment = 0x10000;
  using Event = Profiler::FrameEvent;
  using namespace std::chrono_literals;
  const auto* mode_env = std::getenv("KYTY_COHERENCE_COPY");
  const std::string mode = mode_env != nullptr ? mode_env : "";
  Require(name, "mode", mode == "1" || mode == "verify" || mode == "read" || mode == "read-verify", "fixture needs F1 enabled");
  Profiler::Detail::g_event_sink.store(Profiler::Detail::CounterSink::Thread);
  const auto jobs_before = Profiler::FrameEventTotal(Event::CoherenceCopyJobs);
  const auto guards_before = Profiler::FrameEventTotal(Event::CoherenceCopyGuardWaits);
  const auto reused_before = Profiler::FrameEventTotal(Event::CoherenceCopyRangeReuses);
  const auto mismatches_before = Profiler::FrameEventTotal(Event::CoherenceCopyVerifyMismatches);
  const auto gpu_checks_before = Profiler::FrameEventTotal(Event::CoherenceCopyVerifyGpuChecks);

  auto owner = MakeRenderContext();
  auto &context = *owner;
  auto &cache = context.GetBufferCache();
  auto &scheduler = context.GetCommandScheduler();
  context.InitializeGpu(nullptr);
  LibKernel::Memory::InstallGpuResources(&context);
  std::unique_ptr<CommandProcessor> processor;
  OnGpuThread(context, [&] {
    processor = std::make_unique<CommandProcessor>(context, 0);
    processor->BufferInit();
  });
  int64_t direct_offset = -1;
  Require(name, "allocate", LibKernel::Memory::KernelAllocateDirectMemory(
      0, LibKernel::Memory::KernelGetDirectMemorySize(), bytes, alignment, 0, &direct_offset) == 0,
      "direct allocation failed");
  void *mapped = reinterpret_cast<void *>(base);
  Require(name, "map", LibKernel::Memory::KernelMapDirectMemory(
      &mapped, bytes, 0x3, 0x10, direct_offset, alignment) == 0 && mapped == reinterpret_cast<void *>(base),
      "fixed direct mapping failed");
  void* alias = reinterpret_cast<void*>(base + 0x400000);
  // Reverse two half-mappings: one WRITE_DATA spans mappings whose physical aliases differ.
  const auto alias_address = reinterpret_cast<uint64_t>(alias);
  void* alias_second = reinterpret_cast<void*>(alias_address + bytes / 2);
  Require(name, "shared alias", LibKernel::Memory::KernelMapDirectMemory(
      &alias, bytes / 2, 0x3, 0x10, direct_offset + bytes / 2, alignment) == 0 &&
      LibKernel::Memory::KernelMapDirectMemory(
          &alias_second, bytes / 2, 0x3, 0x10, direct_offset, alignment) == 0,
      "split shared views of the direct allocation failed");
  Require(name, "canonical backing",
      LibKernel::Memory::GuestBackingAlias(alias_address, bytes) == nullptr &&
      LibKernel::Memory::GuestBackingAlias(alias_address, bytes / 2) ==
          LibKernel::Memory::GuestBackingAlias(base + bytes / 2, bytes / 2) &&
      LibKernel::Memory::GuestBackingAlias(alias_address + bytes / 2, bytes / 2) ==
          LibKernel::Memory::GuestBackingAlias(base, bytes / 2),
      "fixture did not span noncontiguous views of one backing allocation");
  std::memset(mapped, 0x19, bytes);
  auto* copier = OnGpuThread(context, [&] {
    return &context.GetTextureCache().EnsureStagingCopier();
  });
  copier->HoldWorkerForTest(true);
  BufferId id{};
  OnGpuThread(context, [&] {
    context.MapMemory(base, bytes);
    id = cache.FindBuffer(base, bytes);
  });
  const bool read_bindings = mode == "read" || mode == "read-verify";
  const auto prepare_upload = [&] {
    if (read_bindings) {
      (void)cache.ObtainBuffer(base, bytes, false, false, id);
    } else {
      context.PrepareBda();
    }
  };
  // A foreign publication can start before the first F1 job has installed its copier pointer.
  // Its lease must keep admission out until all backing bytes have landed.
  std::vector<uint8_t> initial(bytes, 0x41);
  auto publication = context.PrepareHostBackingWrite(base, bytes, RenderContext::HostWriter::Occlusion);
  std::promise<void> preparing;
  auto admission = std::async(std::launch::async, [&] {
    OnGpuThread(context, [&] {
      preparing.set_value();
      prepare_upload();
    });
  });
  preparing.get_future().wait();
  const bool publication_blocks_admission = admission.wait_for(25ms) == std::future_status::timeout;
  const bool unpublished = Profiler::FrameEventTotal(Event::CoherenceCopyJobs) == jobs_before;
  const bool initial_written = LibKernel::Memory::TryWriteBacking(base, initial.data(), bytes);
  publication.unlock();
  admission.get();
  Require(name, "publication before first admission", publication_blocks_admission &&
      unpublished && initial_written, "a source was admitted while its backing publication still held the lease");
  Require(name, "admitted job", Profiler::FrameEventTotal(Event::CoherenceCopyJobs) > jobs_before,
      "the fixture did not use FinishBdaBatchedUpload's host worker");
  auto &buffer = cache.GetBuffer(id);
  const auto snapshot = [&](auto &readback) {
    OnGpuThread(context, [&] {
      const vk::BufferCopy copy{buffer.Offset(base), 0, bytes};
      auto &command = scheduler.Current();
      command.EndRendering();
      command.Handle().copyBuffer(buffer.Handle(), readback.buffer, 1, &copy);
      vk::BufferMemoryBarrier barrier{};
      barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
      barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.buffer = readback.buffer;
      barrier.size = bytes;
      command.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
          vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &barrier, 0, nullptr);
    });
  };
  const auto check = [&](auto &readback, uint32_t expected, const char *stage) {
    const auto actual = ReadBuffer(name, readback, bytes / sizeof(uint32_t));
    Require(name, stage, std::all_of(actual.begin(), actual.end(),
        [=](auto value) { return value == expected; }), "native bytes have the wrong source version");
    DestroyBuffer(&readback);
  };

  auto first = CreateHostBuffer(name, bytes, vk::BufferUsageFlagBits::eTransferDst, {0});
  snapshot(first); // before the later CP overwrite, in this same recording
  OnGpuThread(context, [&] {
    Live::Testing::StageText("KYTY_COHERENCE_COPY=0\n");
    Live::OnCpFlip();
  });
  std::vector<uint32_t> later(bytes / sizeof(uint32_t), 0x72727272);
  auto write = std::async(std::launch::async, [&] {
    OnGpuThread(context, [&] {
      processor->WriteData(static_cast<uint32_t *>(alias), later.data(), later.size(), 0);
    });
  });
  const bool blocked = write.wait_for(25ms) == std::future_status::timeout;
  copier->HoldWorkerForTest(false); // always release before asserting/joining
  write.get();
  Require(name, "CP guard after live disable", blocked &&
      Profiler::FrameEventTotal(Event::CoherenceCopyGuardWaits) > guards_before,
      "WRITE_DATA through split aliases overwrote a queued source after live disable");
  OnGpuThread(context, [&] {
    (void)context.InvalidateMemory(base, bytes); // ordinary cache alias tracking is separate
    SyncEpoch::Advance();
    prepare_upload(); // switch 0, second source version is copied synchronously
  });
  auto second = CreateHostBuffer(name, bytes, vk::BufferUsageFlagBits::eTransferDst, {0});
  snapshot(second);
  OnGpuThread(context, [&] { scheduler.Finish(); });
  check(first, 0x41414141, "earlier upload bytes");
  check(second, 0x72727272, "later upload bytes");

  // A submission must not consume staging before its CPU copy. Gate the copier, then start
  // Finish on another host thread so the fixture can release it without any GPU dependency.
  OnGpuThread(context, [&] {
    Live::Testing::StageText(std::string("KYTY_COHERENCE_COPY=") + mode + "\n");
    Live::OnCpFlip();
    std::fill(later.begin(), later.end(), 0x33333333);
    auto lease = context.PrepareHostBackingWrite(base, bytes, RenderContext::HostWriter::Occlusion);
    LibKernel::Memory::WriteBacking(base, later.data(), bytes);
  });
  copier->HoldWorkerForTest(true);
  OnGpuThread(context, [&] {
    SyncEpoch::Advance();
    prepare_upload();
  });
  auto third = CreateHostBuffer(name, bytes, vk::BufferUsageFlagBits::eTransferDst, {0});
  snapshot(third);
  auto finish = std::async(std::launch::async, [&] {
    OnGpuThread(context, [&] { scheduler.Finish(); });
  });
  const bool submit_blocked = finish.wait_for(25ms) == std::future_status::timeout;
  copier->HoldWorkerForTest(false);
  finish.get();
  Require(name, "pending submit", submit_blocked, "submission completed while its source was held");
  check(third, 0x33333333, "host dependency upload bytes");

  // A waiting foreign writer must recheck after ticket A: ticket B may be admitted meanwhile.
  copier->HoldWorkerForTest(true);
  const auto dirty_and_prepare = [&] {
    OnGpuThread(context, [&] {
      (void)context.InvalidateMemory(base, bytes);
      SyncEpoch::Advance();
      prepare_upload();
    });
  };
  dirty_and_prepare();
  const auto ticket_a = copier->PendingValue();
  std::promise<void> writer_started;
  auto publication_write = std::async(std::launch::async, [&] {
    writer_started.set_value();
    auto lease = context.PrepareHostBackingWrite(base, bytes, RenderContext::HostWriter::Occlusion);
    return LibKernel::Memory::TryWriteBacking(base, later.data(), bytes);
  });
  writer_started.get_future().wait();
  const bool waits_a = publication_write.wait_for(25ms) == std::future_status::timeout;
  dirty_and_prepare();
  const auto ticket_b = copier->PendingValue();
  copier->HoldWorkerAfterForTest(ticket_a);
  copier->HoldWorkerForTest(false);
  copier->WaitHost(ticket_a); // worker completes A and deliberately leaves B pending
  const bool waits_b = publication_write.wait_for(25ms) == std::future_status::timeout;
  copier->HoldWorkerAfterForTest(0); // always release before asserting/joining
  const bool published = publication_write.get();
  OnGpuThread(context, [&] { scheduler.Finish(); });
  Require(name, "publication rechecks new producer", ticket_a != 0 && ticket_b > ticket_a &&
      waits_a && waits_b && published, "a waiting publication skipped a newly admitted overlapping source");

  // A collected run spanning mappings has no contiguous alias: retain the inline fallback.
  Require(name, "fallback lacks contiguous alias", LibKernel::Memory::GuestBackingAlias(
      alias_address, bytes) == nullptr, "fallback unexpectedly had one contiguous backing alias");
  auto fallback_readback = CreateHostBuffer(name, bytes, vk::BufferUsageFlagBits::eTransferDst, {0});
  const auto fallback_jobs_before = Profiler::FrameEventTotal(Event::CoherenceCopyJobs);
  OnGpuThread(context, [&] {
    context.MapMemory(alias_address, bytes);
    const auto fallback_id = cache.FindBuffer(alias_address, bytes);
    SyncEpoch::Advance();
    if (read_bindings) {
      (void)cache.ObtainBuffer(alias_address, bytes, false, false, fallback_id);
    } else {
      context.PrepareBda();
    }
    auto& command = scheduler.Current();
    command.EndRendering();
    const auto& fallback_buffer = cache.GetBuffer(fallback_id);
    const vk::BufferCopy copy{fallback_buffer.Offset(alias_address), 0, bytes};
    const auto native = command.Handle();
    native.copyBuffer(fallback_buffer.Handle(), fallback_readback.buffer, 1, &copy);
    vk::MemoryBarrier barrier{};
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
    native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eHost, {}, 1, &barrier, 0, nullptr, 0, nullptr);
    scheduler.Finish();
  });
  const auto fallback_data = ReadBuffer(name, fallback_readback, bytes / sizeof(uint32_t));
  Require(name, "inline fallback bytes",
      Profiler::FrameEventTotal(Event::CoherenceCopyJobs) == fallback_jobs_before &&
      std::all_of(fallback_data.begin(), fallback_data.end(), [](auto value) { return value == 0x33333333; }),
      "an upload without a contiguous alias was deferred or its bytes changed");
  DestroyBuffer(&fallback_readback);
  OnGpuThread(context, [&] { context.UnmapMemory(alias_address, bytes); });

  // Kernel unmap retains aliases until recording and its source copies have drained.
  OnGpuThread(context, [&] {
    auto lease = context.PrepareHostBackingWrite(base, bytes, RenderContext::HostWriter::Occlusion);
    LibKernel::Memory::WriteBacking(base, later.data(), bytes);
  });
  copier->HoldWorkerForTest(true);
  OnGpuThread(context, [&] {
    SyncEpoch::Advance();
    prepare_upload();
  });
  auto unmap = std::async(std::launch::async, [&] {
    return LibKernel::Memory::KernelMunmap(base, bytes);
  });
  const bool unmap_blocked = unmap.wait_for(25ms) == std::future_status::timeout;
  copier->HoldWorkerForTest(false);
  const auto unmapped = unmap.get();
  Require(name, "unmap drains pending source", unmap_blocked && unmapped == 0,
      "kernel unmap did not preserve pending copy lifetime");
  OnGpuThread(context, [&] { processor.reset(); });
  Require(name, "unmap alias", LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(alias), bytes) == 0,
      "shared alias unmap failed");
  context.ShutdownGpu();
  LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name, "release", LibKernel::Memory::KernelReleaseDirectMemory(direct_offset, bytes) == 0,
      "direct allocation release failed");
  owner.reset();
  Require(name, "verify", Profiler::FrameEventTotal(Event::CoherenceCopyVerifyMismatches) == mismatches_before,
      "source or GPU verification found a mismatch");
  Require(name, "range storage reused", Profiler::FrameEventTotal(Event::CoherenceCopyRangeReuses) >
      reused_before, "completed F1 jobs discarded their reusable range storage");
  if (mode == "verify" || mode == "read-verify")
    Require(name, "GPU verification ran", Profiler::FrameEventTotal(Event::CoherenceCopyVerifyGpuChecks) >
        gpu_checks_before, "verify mode omitted native readback comparison");
  std::printf("[gpu]     %-32s ok (mode=%s)\n", name, mode.c_str());
}
