// Native timeline completion is insufficient for a guest publication. The
// collection of a later producer must progress while an earlier label is gated.
void CheckDeferredCoherenceScheduler() {
  constexpr const char* name = "DeferredCoherenceScheduler";
  using namespace std::chrono_literals;
  EnsureRuntimeContext();
  CommandScheduler scheduler(Renderer(), m_runtime_context, CommandScheduler::Role::Guest);
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  scheduler.Begin(registers, user_config, shaders);
  struct State {
    std::atomic<uint64_t> applied{0};
    std::atomic<uint32_t> collected{0}, published{0};
    bool may_apply = false; // owner only
  } state;
  scheduler.SetCoherenceHooks(
      [](void* context) { return static_cast<State*>(context)->applied.load(std::memory_order_acquire); },
      [](void* context) {
        auto& state = *static_cast<State*>(context);
        if (state.may_apply && state.collected.load(std::memory_order_acquire) == 3)
          state.applied.store(2, std::memory_order_release);
      }, &state);
  const auto a_tick = scheduler.CurrentTick();
  scheduler.SetCoherencePrefix(1);
  scheduler.DeferCollectionOperation([&] { state.collected.fetch_or(1, std::memory_order_release); }, a_tick);
  scheduler.DeferPriorityOperation([&] { state.published.fetch_or(1, std::memory_order_release); });
  scheduler.Flush();
  const auto b_tick = scheduler.CurrentTick();
  scheduler.SetCoherencePrefix(2);
  scheduler.DeferCollectionOperation([&] { state.collected.fetch_or(2, std::memory_order_release); }, b_tick);
  scheduler.DeferPriorityOperation([&] { state.published.fetch_or(2, std::memory_order_release); });
  uint32_t retired = 0;
  scheduler.DeferOperation([&] { ++retired; });
  scheduler.Flush();
  scheduler.Wait(b_tick);
  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  while (state.collected.load(std::memory_order_acquire) != 3 &&
         std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  const bool collected_independently = state.collected.load(std::memory_order_acquire) == 3;
  const bool publication_held = state.published.load(std::memory_order_acquire) == 0;
  scheduler.PopReadyOperations();
  const bool retirement_held = retired == 0;
  state.applied.store(1, std::memory_order_release);
  scheduler.NotifyCoherenceApplied();
  const auto a_deadline = std::chrono::steady_clock::now() + 500ms;
  while (state.published.load(std::memory_order_acquire) == 0 &&
         std::chrono::steady_clock::now() < a_deadline) std::this_thread::yield();
  const bool only_a_published = state.published.load(std::memory_order_acquire) == 1;
  // Always release B before assertions so a faulty gate cannot hang teardown.
  state.may_apply = true;
  scheduler.ServiceCoherence();
  state.applied.store(2, std::memory_order_release);
  scheduler.NotifyCoherenceApplied();
  scheduler.Finish();
  scheduler.DrainPriorityOperations();
  scheduler.SetCoherenceHooks(nullptr, nullptr, nullptr);
  Require(name, "independent collection", collected_independently,
          "a pending label blocked collection of a later producer");
  Require(name, "native versus applied", publication_held && retirement_held,
          "native completion published guest state or retired storage before owner apply");
  Require(name, "captured prefix", only_a_published,
          "label A waited for unrelated B or label B was released with A");
  Require(name, "drain", state.published.load() == 3 && retired == 1,
          "drain lost an applied publication or retained resource");
  std::printf("[host]    %-32s ok\n", name);
}

// A native semaphore holds A's completion. The owner must return after recording;
// its retained domain must already reject a clean backing read. B overlaps A and
// waits on a second semaphore, proving that application and publication of A do
// not wait for B. CPU readback occurs only after both relevant producers apply.
void CheckDeferredBdaWrites() {
  constexpr const char* name = "DeferredBdaWrites";
  constexpr uint64_t base = 0x0000000212000000ull;
  constexpr uint64_t allocation = 0x10000, bytes = BufferCache::CACHING_PAGESIZE;
  using namespace std::chrono_literals;
  auto owner = MakeRenderContext();
  auto& context = *owner;
  auto& cache = context.GetBufferCache();
  auto& scheduler = context.GetCommandScheduler();
  context.InitializeGpu(nullptr);
  LibKernel::Memory::InstallGpuResources(&context);
  std::unique_ptr<CommandProcessor> processor;
  OnGpuThread(context, [&] {
    processor = std::make_unique<CommandProcessor>(context, 0);
    processor->BufferInit();
  });
  int64_t offset = -1;
  Require(name, "allocate", LibKernel::Memory::KernelAllocateDirectMemory(
      0, LibKernel::Memory::KernelGetDirectMemorySize(), allocation, allocation, 0, &offset) == 0,
      "direct allocation failed");
  void* mapped = reinterpret_cast<void*>(base);
  Require(name, "map", LibKernel::Memory::KernelMapDirectMemory(
      &mapped, allocation, 0x3, 0x10, offset, allocation) == 0, "direct mapping failed");
  std::memset(mapped, 0x19, allocation);
  vk::SemaphoreTypeCreateInfo type{};
  type.semaphoreType = vk::SemaphoreType::eTimeline;
  vk::SemaphoreCreateInfo info{};
  info.pNext = &type;
  const auto gate = m_runtime_context.device.createSemaphore(info).value;
  Require(name, "native gate", gate != nullptr, "timeline creation failed");
  BufferId id{};
  OnGpuThread(context, [&] {
    context.MapMemory(base, allocation);
    id = cache.FindBuffer(base, bytes);
    (void)cache.FindBuffer(base + bytes * 2, bytes);
    context.PrepareBda();
    scheduler.Finish();
  });
  const auto write = [&](uint32_t value) {
    auto& command = scheduler.Current();
    const auto& buffer = cache.GetBuffer(id);
    command.Sink().fillBuffer(buffer.Handle(), buffer.Offset(base), bytes, value);
    const auto page = base / bytes;
    // Transfer writes model the emitter's atomic bitmap OR. The real GPU
    // compactor, host invalidation, ledger and owner application all run.
    command.Sink().fillBuffer(cache.GetFaultBuffer()->Handle(),
        BufferCache::BDA_WRITE_BITMAP_OFFSET + (page / 32) * sizeof(uint32_t),
        sizeof(uint32_t), 1u << (page % 32));
    vk::MemoryBarrier barrier{};
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
    command.Sink().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader, {}, 1, &barrier, 0, nullptr, 0, nullptr);
  };
  uint32_t guest_label = 0;
  bool label_was_held = false;
  std::atomic<uint32_t> labels{0};
  uint64_t a = 0, b = 0, a_tick = 0, b_tick = 0;
  const auto shader_hash = uint64_t{0x1234};
  // Admission needs a confirmed write history: synchronous settles of the same page.
  OnGpuThread(context, [&] {
    for (int settle = 0; settle < 3; ++settle) {
      Require(name, "history", !cache.PrepareDeferredBdaWrite(shader_hash),
              "a writer was admitted before its write history was confirmed");
      write(0x5a5a5a5a);
      cache.SettleBdaWrites(shader_hash);
    }
  });
  const auto start = std::chrono::steady_clock::now();
  OnGpuThread(context, [&] {
    Require(name, "prepare A", cache.PrepareDeferredBdaWrite(shader_hash), "destination proof was refused");
    a = cache.BeginDeferredBdaWrite(shader_hash);
    write(0xa5a5a5a5);
    cache.QueueDeferredBdaWrite(a);
    a_tick = scheduler.CurrentTick();
    scheduler.DeferPriorityOperation([&] { labels.fetch_or(1, std::memory_order_release); });
    SubmitInfo wait;
    wait.AddWait(gate, 1);
    scheduler.Flush(wait);
  });
  OnGpuThread(context, [&] {
    const auto address = reinterpret_cast<uint64_t>(&guest_label);
    const std::array<uint32_t, 8> release{
        KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_RELEASE_MEM), 0x28u | (5u << 8u),
        1u << 29u, static_cast<uint32_t>(address), static_cast<uint32_t>(address >> 32),
        0xd4u, 0u, 0u};
    Pm4Execution execution;
    Require(name, "label admission", processor->Process(execution, release) == Pm4ProcessResult::Complete,
            "recording an ordered label blocked the CP");
    label_was_held = guest_label == 0;
  });
  const bool recording_returned = std::chrono::steady_clock::now() - start < 1s;
  const bool protected_before_native = OnGpuThread(context, [&] {
    uint32_t value = 0;
    cache.RecordKnownFill(base + bytes * 2, 4, 0x77);
    return a != 0 && cache.PendingBdaWrite(base, 4) == a &&
        !LibKernel::Memory::TryReadGpuCleanBacking(base, &value, sizeof(value)) &&
        !cache.KnownFill(base + bytes * 2, 4);
  });
  OnGpuThread(context, [&] {
    Require(name, "prepare B", cache.PrepareDeferredBdaWrite(shader_hash), "overlapping destination proof was refused");
    b = cache.BeginDeferredBdaWrite(shader_hash);
    write(0xb6b6b6b6);
    cache.QueueDeferredBdaWrite(b);
    b_tick = scheduler.CurrentTick();
    scheduler.DeferPriorityOperation([&] { labels.fetch_or(2, std::memory_order_release); });
    SubmitInfo wait;
    wait.AddWait(gate, 2);
    scheduler.Flush(wait);
  });
  // A later known writer must remain authoritative when B applies.
  OnGpuThread(context, [&] {
    cache.FillBuffer(base, 4, 0xc7c7c7c7, false);
    scheduler.Flush();
  });
  vk::SemaphoreSignalInfo signal{};
  signal.semaphore = gate;
  signal.value = 1;
  RequireVk(name, "release A", m_runtime_context.device.signalSemaphore(&signal), "signal A");
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (labels.load(std::memory_order_acquire) == 0 &&
         std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  const bool only_a = labels.load(std::memory_order_acquire) == 1;
  // The runner queues the real label write to the GPU owner after the synthetic A marker.
  // Wait for that separate handoff while B remains held at its native semaphore.
  bool b_retained = false;
  const auto owner_label_deadline = std::chrono::steady_clock::now() + 1s;
  while (!b_retained && std::chrono::steady_clock::now() < owner_label_deadline) {
    b_retained = OnGpuThread(context, [&] {
      return guest_label == 0xd4u && cache.PendingBdaWrite(base, 4) == b && cache.HasPendingBdaWrites();
    });
    if (!b_retained) std::this_thread::yield();
  }
  const auto lo = [](uint64_t address) { return static_cast<uint32_t>(address); };
  const auto hi = [](uint64_t address) { return static_cast<uint32_t>(address >> 32); };
  constexpr uint64_t pending_label_address = base + bytes;
  const std::array<uint32_t, 8> pending_release{
      KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_RELEASE_MEM), 0x28u | (5u << 8u),
      1u << 29u, lo(pending_label_address), hi(pending_label_address), 0xd5u, 0u, 0u};
  const std::array<uint32_t, 5> known_write{
      KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0), 0u,
      lo(pending_label_address), hi(pending_label_address), 0xf6u};
  Pm4Execution known_execution;
  const bool label_writer_suspended = OnGpuThread(context, [&] {
    Pm4Execution release_execution;
    Require(name, "pending label", processor->Process(release_execution, pending_release) ==
        Pm4ProcessResult::Complete, "label recording waited for a pending producer");
    uint32_t label_value = 0;
    const bool probe_held = !LibKernel::Memory::TryReadGpuCleanBacking(
        pending_label_address, &label_value, sizeof(label_value));
    return probe_held && processor->Process(known_execution, known_write) == Pm4ProcessResult::Blocked &&
           !known_execution.MadeProgress();
  });
  uint32_t cp_marker = 0;
  const auto marker_address = reinterpret_cast<uint64_t>(&cp_marker);
  const std::array<uint32_t, 10> cp_stream{
      KYTY_PM4(5, Pm4::IT_COND_EXEC, 0), lo(base), hi(base), 0u, 5u,
      KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0), 0u, lo(marker_address), hi(marker_address), 0xc3u};
  Pm4Execution cp_execution;
  std::atomic<bool> cp_returned{false};
  bool cp_suspended = false;
  bool shader_suspended = false;
  bool vertex_tables_suspended = false;
  std::jthread cp_reader([&] {
    OnGpuThread(context, [&] {
      HW::ComputeShaderInfo regs{};
      regs.cs_regs.data_addr = base;
      ShaderComputeInputInfo input{};
      PipelineCache::StagePrep prep;
      context.ClearDeferredGpuRead();
      const auto program = context.GetPipelineCache().GetComputeProgram(
          regs, HW::ShaderRegisters{}, input, prep);
      shader_suspended = !program && context.DeferredGpuRead();
      // The shader and its AGC metadata are clean; only SGPR-derived tables are pending.
      static const std::array<uint32_t, 2> vertex_code{0xbf810000u, 0xbf810000u};
      constexpr size_t direct_count = static_cast<size_t>(AgcDirectResourceType::Last) + 1;
      std::array<uint16_t, direct_count> offsets;
      offsets.fill(AGC_ILLEGAL_DIRECT_OFFSET);
      offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexAttribDescTable)] = 0;
      offsets[static_cast<size_t>(AgcDirectResourceType::PtrVertexBufferTable)] = 2;
      ShaderUserData user_data{};
      user_data.direct_resource_count = direct_count;
      user_data.direct_resource_offset = offsets.data();
      ShaderSemantic semantic{};
      semantic.size_in_elements = 1;
      const auto code_address = reinterpret_cast<uint64_t>(vertex_code.data());
      ShaderMapUserData(code_address,
          {.type = Prospero::ShaderBinaryType::kGs, .user_data = &user_data,
           .input_semantics = &semantic, .num_input_semantics = 1,
           .code_size_bytes = sizeof(vertex_code)});
      HW::VertexShaderInfo vertex_regs{};
      vertex_regs.es_regs.data_addr = code_address;
      vertex_regs.gs_regs.rsrc2.user_sgpr = 4;
      vertex_regs.gs_user_sgpr.value[0] = lo(base);
      vertex_regs.gs_user_sgpr.value[1] = hi(base);
      vertex_regs.gs_user_sgpr.value[2] = lo(base);
      vertex_regs.gs_user_sgpr.value[3] = hi(base);
      ShaderVertexInputInfo vertex_input{};
      context.ClearDeferredGpuRead();
      (void)PrepareProgram(vertex_regs, HW::Context{}, HW::UserConfig{}, vertex_input);
      const bool attributes_suspended = context.DeferredGpuRead();
      const uint32_t clean_attribute = 0;
      const auto attribute_address = reinterpret_cast<uint64_t>(&clean_attribute);
      vertex_regs.gs_user_sgpr.value[0] = lo(attribute_address);
      vertex_regs.gs_user_sgpr.value[1] = hi(attribute_address);
      context.ClearDeferredGpuRead();
      (void)PrepareProgram(vertex_regs, HW::Context{}, HW::UserConfig{}, vertex_input);
      vertex_tables_suspended = attributes_suspended && context.DeferredGpuRead();
      cp_suspended = processor->Process(cp_execution, cp_stream) == Pm4ProcessResult::Blocked &&
                     !cp_execution.MadeProgress() && cp_marker == 0;
    });
    cp_returned.store(true, std::memory_order_release);
  });
  const auto cp_deadline = std::chrono::steady_clock::now() + 500ms;
  while (!cp_returned.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < cp_deadline) std::this_thread::yield();
  const bool cp_did_not_wait = cp_returned.load(std::memory_order_acquire);
  bool independent_queue_advanced = false;
  uint32_t graphics_marker = 0, compute_marker = 0;
  const auto graphics_address = reinterpret_cast<uint64_t>(&graphics_marker);
  const auto compute_address = reinterpret_cast<uint64_t>(&compute_marker);
  const std::array<uint32_t, 10> graphics_stream{
      KYTY_PM4(7, Pm4::IT_WAIT_REG_MEM, 0), 0x13u, lo(base), hi(base),
      0xc7c7c7c7u, UINT32_MAX, 0u,
      0x80000000u, 0x80000000u, 0x80000000u};
  const std::array<uint32_t, 5> graphics_suffix{
      KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0), 0u, lo(graphics_address), hi(graphics_address), 0xe5u};
  std::vector<uint32_t> graphics_commands(graphics_stream.begin(), graphics_stream.end());
  graphics_commands.insert(graphics_commands.end(), graphics_suffix.begin(), graphics_suffix.end());
  const std::array<uint32_t, 5> compute_stream{
      KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0), 0u, lo(compute_address), hi(compute_address), 0xf6u};
  if (cp_did_not_wait) {
    if (cp_reader.joinable()) cp_reader.join();
    context.GetGpu().Submit(graphics_commands, {});
    context.GetGpu().SubmitCompute(0x20, compute_stream);
    const auto independent_deadline = std::chrono::steady_clock::now() + 500ms;
    while (std::chrono::steady_clock::now() < independent_deadline) {
      independent_queue_advanced = OnGpuThread(context, [&] {
        return compute_marker == 0xf6u && graphics_marker == 0;
      });
      if (independent_queue_advanced) break;
      std::this_thread::yield();
    }
  }
  // Release the native producer even on a regression, so the read and teardown finish.
  signal.value = 2;
  RequireVk(name, "release B", m_runtime_context.device.signalSemaphore(&signal), "signal B");
  if (cp_reader.joinable()) cp_reader.join();
  uint32_t actual = 0;
  // The production raw-fault path must wait and publish before unprotecting.
  std::memcpy(&actual, mapped, sizeof(actual));
  OnGpuThread(context, [&] {
    scheduler.Finish();
    scheduler.DrainPriorityOperations();
    Require(name, "both applied", !cache.HasPendingBdaWrites(), "drain left a pending producer");
  });
  context.GetGpu().Done();
  context.GetGpu().Done();
  const bool known_writer_retained = OnGpuThread(context, [&] {
    // No other CP stream is active after Done's captured submission prefix drains.
    if (cp_suspended) {
      Require(name, "CP retry", processor->Process(cp_execution, cp_stream) == Pm4ProcessResult::Complete,
              "applied pages did not resume the suspended packet");
    }
    // Applied can precede a queued label handoff. Exercise that remaining-label
    // state with the actual registration API, without any unknown producer.
    auto& gpu = context.GetGpu();
    const auto label_tick = scheduler.CurrentTick();
    cache.RecordKnownFill(pending_label_address, 4, 0xf6u);
    gpu.AddDeferredLabel(pending_label_address, 4, label_tick);
    const bool fill_held = !cache.KnownFill(pending_label_address, 4);
    uint32_t value = 0;
    const bool exact_held = !LibKernel::Memory::TryReadGpuCleanBacking(
        pending_label_address, &value, sizeof(value));
    const bool certificate_held = !LibKernel::Memory::IsGpuCleanForRead(pending_label_address, 4);
    DrawPrep::ReadSet reads;
    DrawPrep::Recorder recorder{.reads = &reads, .exact = false};
    bool worker_held = false;
    {
      DrawPrep::RecordScope scope(recorder);
      worker_held = !LibKernel::Memory::TryReadGpuCleanBacking(
          pending_label_address, &value, sizeof(value)) && reads.Failed();
    }
    gpu.RemoveDeferredLabel(pending_label_address, label_tick);
    Require(name, "label versus clean certificates", exact_held && certificate_held && worker_held && fill_held,
            "a pending label passed a probe, certificate or known fill after Applied");
    static const std::array<uint32_t, 2> code{0xbf810000u, 0xbf810000u};
    ShaderUserData metadata{};
    const auto code_address = reinterpret_cast<uint64_t>(code.data());
    const auto metadata_address = reinterpret_cast<uint64_t>(&metadata);
    ShaderMapUserData(code_address,
        {.type = Prospero::ShaderBinaryType::kGs, .user_data = &metadata,
         .code_size_bytes = sizeof(code)});
    gpu.AddDeferredLabel(metadata_address, 4, label_tick);
    context.ClearDeferredGpuRead();
    const bool metadata_held = ShaderDeferPendingReads(code_address) && context.DeferredGpuRead();
    gpu.RemoveDeferredLabel(metadata_address, label_tick);
    context.ClearDeferredGpuRead();
    Require(name, "label versus shader metadata", metadata_held,
            "clean shader metadata bypassed readiness with only a pending label");
    return cache.KnownFill(base, 4) == 0xc7c7c7c7u && graphics_marker == 0xe5u;
  });
  OnGpuThread(context, [&] {
    if (DrawPrep::GetMode() != DrawPrep::Mode::Parallel) return;
    DrawPrep::Engine engine(context, {}, {});
    DrawAutoArgs args{};
    const auto before = DrawPrep::GetTotals().head_self.load();
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      const auto position = engine.Publish(nullptr, &args, processor->GetCtx(),
          processor->GetUcfg(), processor->GetShCtx(), [] { return false; });
      context.RequestDeferredGpuRead();
      engine.CommitPublished(position, 0, 1);
      Require(name, "retained draw", engine.Pending(), "a suspended slot was retired");
      context.ClearDeferredGpuRead();
      engine.CommitPublished(position, 0, 1);
      Require(name, "draw retry", !engine.Pending(), "a retained head did not retire after retry");
      if (DrawPrep::GetTotals().head_self.load() != before) break;
    }
    Require(name, "self-prepared retry", DrawPrep::GetTotals().head_self.load() != before,
            "the retry fixture did not exercise an owner-claimed head");
  });
  bool known_write_finished = false;
  const auto label_deadline = std::chrono::steady_clock::now() + 1s;
  while (!known_write_finished && std::chrono::steady_clock::now() < label_deadline) {
    known_write_finished = OnGpuThread(context, [&] {
      return processor->Process(known_execution, known_write) == Pm4ProcessResult::Complete;
    });
    if (!known_write_finished) std::this_thread::yield();
  }
  uint32_t ordered_value = 0;
  std::memcpy(&ordered_value, reinterpret_cast<const void*>(pending_label_address), sizeof(ordered_value));
  Require(name, "known writer after label", label_writer_suspended &&
      known_write_finished && ordered_value == 0xf6u, "a deferred label overwrote its later known writer");
  void* alias = reinterpret_cast<void*>(base + 0x400000);
  Require(name, "alias after apply", LibKernel::Memory::KernelMapDirectMemory(
      &alias, allocation, 0x3, 0x10, offset, allocation) == 0,
      "new alias did not drain and publish retained physical bytes");
  const auto alias_value = *static_cast<const uint32_t*>(alias);
  const bool alias_refused = OnGpuThread(context, [&] {
    return !cache.PrepareDeferredBdaWrite(shader_hash);
  });
  Require(name, "unmap alias", LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(alias), allocation) == 0,
          "alias unmap failed");
  Require(name, "unmap owner", LibKernel::Memory::KernelMunmap(base, allocation) == 0,
          "owner unmap failed");
  Require(name, "release", LibKernel::Memory::KernelReleaseDirectMemory(offset, allocation) == 0,
          "direct release failed");
  OnGpuThread(context, [&] {
    // Inject the compactor's finite-list boundary without allocating a 1 GiB guest domain.
    FaultManager faults(m_runtime_context, scheduler, cache);
    const auto handle = faults.GetFaultBuffer()->Handle();
    auto command = scheduler.Current().Sink();
    command.fillBuffer(handle, BufferCache::BDA_WRITE_BITMAP_OFFSET,
                       65536u / 8u, UINT32_MAX);
    command.fillBuffer(handle, BufferCache::BDA_DROPPED_WRITES_OFFSET, 4, 1);
    vk::MemoryBarrier barrier{};
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader, {}, 1, &barrier, 0, nullptr, 0, nullptr);
    FaultManager::BdaWrites result;
    scheduler.Wait(faults.RecordBdaWrites(0));
    faults.ParseBdaWrites(0, result);
    uint64_t retained_pages = 0;
    result.written.ForEach([&](uint64_t begin, uint64_t end) { retained_pages += (end - begin) / bytes; });
    Require(name, "collector overflow/drop", result.pages == 65536 && result.overflow &&
        result.dropped == 1 && retained_pages == 65535,
        "the compacted list hid overflow or a dropped write");
    faults.ReleaseBdaWrites(0);
    scheduler.Wait(faults.RecordBdaWrites(0));
    faults.ParseBdaWrites(0, result);
    Require(name, "collector slot reuse", result.pages == 0 && !result.overflow && result.dropped == 0,
            "a reused collection slot retained its old bitmap or dropped count");
    faults.ReleaseBdaWrites(0);
    processor.reset();
  });
  context.ShutdownGpu();
  LibKernel::Memory::InstallGpuResources(nullptr);
  m_runtime_context.device.destroySemaphore(gate);
  Require(name, "nonblocking recording", recording_returned && protected_before_native && label_was_held,
          "recording waited for native completion or exposed an unprotected destination");
  if (!only_a || !b_retained) std::fprintf(stderr,
      "DeferredBdaWrites publication diagnostics: only_a=%d b_retained=%d label=0x%x tickets=%llu/%llu ticks=%llu/%llu\n",
      only_a, b_retained, guest_label, static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
      static_cast<unsigned long long>(a_tick), static_cast<unsigned long long>(b_tick));
  if (!cp_did_not_wait || !cp_suspended || !shader_suspended || !vertex_tables_suspended ||
      !independent_queue_advanced || !known_writer_retained) std::fprintf(stderr,
      "DeferredBdaWrites readiness diagnostics: returned=%d cp=%d shader=%d vertex=%d other_queue=%d known=%d marker=0x%x\n",
      cp_did_not_wait, cp_suspended, shader_suspended, vertex_tables_suspended,
      independent_queue_advanced, known_writer_retained, cp_marker);
  Require(name, "separate producers", only_a && b_retained && b > a && b_tick > a_tick,
          "applying A released B or publication A waited for B");
  Require(name, "CP readiness", cp_did_not_wait && cp_suspended && shader_suspended && vertex_tables_suspended && cp_marker == 0xc3u &&
          independent_queue_advanced && known_writer_retained,

          "CP waited on its own raw page fault, consumed a pending value, or lost the retry");
  Require(name, "raw CPU result", actual == 0xc7c7c7c7 && alias_value == actual,
          "CPU or new physical alias observed stale native buffer bytes");
  Require(name, "alias admission proof", alias_refused,
          "an unprotected physical alias was admitted as a deferred write destination");
  std::printf("[host]    %-32s ok\n", name);
}

// KYTY_BDA_WRITES=deferred admission by write history. A writer settles synchronously until its
// history is confirmed; an admitted execution that writes outside the history is settled when it
// applies (the counted race window), grows the history and demotes the shader until the grown
// history is confirmed again (twice as many settles). A synchronous settle while a deferred
// producer holds result slot 0 uses its own slot. A writer that never writes is admitted with an
// empty domain.
void CheckDeferredBdaHistory() {
  constexpr const char* name = "DeferredBdaHistory";
  constexpr uint64_t base = 0x0000000213000000ull;
  constexpr uint64_t allocation = 0x20000, bytes = BufferCache::CACHING_PAGESIZE;
  constexpr uint64_t page_a = base, page_b = base + bytes * 2, page_c = base + bytes * 4;
  auto owner = MakeRenderContext();
  auto& context = *owner;
  auto& cache = context.GetBufferCache();
  auto& scheduler = context.GetCommandScheduler();
  context.InitializeGpu(nullptr);
  LibKernel::Memory::InstallGpuResources(&context);
  std::unique_ptr<CommandProcessor> processor; // its BufferInit activates recording
  OnGpuThread(context, [&] {
    processor = std::make_unique<CommandProcessor>(context, 0);
    processor->BufferInit();
  });
  int64_t offset = -1;
  Require(name, "allocate", LibKernel::Memory::KernelAllocateDirectMemory(
      0, LibKernel::Memory::KernelGetDirectMemorySize(), allocation, allocation, 0, &offset) == 0,
      "direct allocation failed");
  void* mapped = reinterpret_cast<void*>(base);
  Require(name, "map", LibKernel::Memory::KernelMapDirectMemory(
      &mapped, allocation, 0x3, 0x10, offset, allocation) == 0, "direct mapping failed");
  std::memset(mapped, 0x19, allocation);
  OnGpuThread(context, [&] {
    context.MapMemory(base, allocation);
    for (const auto page : {page_a, page_b, page_c}) (void)cache.FindBuffer(page, bytes);
    context.PrepareBda();
    scheduler.Finish();
  });
  // A shader's writes: the bytes and its written-page bitmap bit, as the emitter records them.
  const auto write = [&](uint64_t page_address, uint32_t value) {
    auto& command = scheduler.Current();
    const auto& buffer = cache.GetBuffer(cache.FindBuffer(page_address, bytes));
    command.Sink().fillBuffer(buffer.Handle(), buffer.Offset(page_address), bytes, value);
    const auto page = page_address / bytes;
    command.Sink().fillBuffer(cache.GetFaultBuffer()->Handle(),
        BufferCache::BDA_WRITE_BITMAP_OFFSET + (page / 32) * sizeof(uint32_t),
        sizeof(uint32_t), 1u << (page % 32));
    vk::MemoryBarrier barrier{};
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
    command.Sink().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader, {}, 1, &barrier, 0, nullptr, 0, nullptr);
  };
  constexpr uint64_t writer = 0x5678, other = 0x9abc, silent = 0xdef0;
  OnGpuThread(context, [&] {
    for (int settle = 0; settle < 3; ++settle) {
      Require(name, "unconfirmed", !cache.PrepareDeferredBdaWrite(writer),
              "a writer was admitted before its history was confirmed");
      write(page_a, 0x11111111);
      cache.SettleBdaWrites(writer);
    }
    Require(name, "confirmed", cache.PrepareDeferredBdaWrite(writer),
            "two settles inside the history did not admit the writer");
    const auto ticket = cache.BeginDeferredBdaWrite(writer);
    write(page_a, 0x22222222);
    write(page_b, 0x33333333); // outside the history
    cache.QueueDeferredBdaWrite(ticket);
    Require(name, "domain", cache.PendingBdaWrite(page_a, 4) == ticket &&
            cache.PendingBdaWrite(page_b, 4) == 0 && !cache.HasGpuDirtyBytes(page_b, 4),
            "the domain is not the history (or a page outside it was claimed before the result)");
    // Phase 0 for another writer while the deferred result still holds slot 0.
    write(page_c, 0x44444444);
    cache.SettleBdaWrites(other);
    Require(name, "sync slot", cache.HasGpuDirtyBytes(page_c, 4),
            "a synchronous settle did not settle its page beside a pending producer");
    scheduler.Finish();
    scheduler.DrainPriorityOperations();
    cache.ServiceDeferredBdaWrites();
    Require(name, "applied", !cache.HasPendingBdaWrites(), "the deferred result was not applied");
    Require(name, "miss settled", cache.HasGpuDirtyBytes(page_b, 4),
            "a page written outside the domain was not settled when the result applied");
    // The grown history needs four confirmations now.
    for (int settle = 0; settle < 4; ++settle) {
      Require(name, "demoted", !cache.PrepareDeferredBdaWrite(writer),
              "a writer that missed its domain was admitted before the grown history was confirmed");
      write(page_a, 0x55555555);
      write(page_b, 0x66666666);
      cache.SettleBdaWrites(writer);
    }
    Require(name, "readmitted", cache.PrepareDeferredBdaWrite(writer),
            "the confirmed grown history did not admit the writer again");
    // A writer that never writes: admitted with an empty domain, its producer only gates labels.
    for (int settle = 0; settle < 2; ++settle) {
      Require(name, "silent unconfirmed", !cache.PrepareDeferredBdaWrite(silent),
              "a silent writer was admitted before its empty history was confirmed");
      cache.SettleBdaWrites(silent);
    }
    Require(name, "silent admitted", cache.PrepareDeferredBdaWrite(silent),
            "a confirmed empty history did not admit the writer");
    const auto empty = cache.BeginDeferredBdaWrite(silent);
    cache.QueueDeferredBdaWrite(empty);
    Require(name, "empty domain", empty != 0 && cache.PendingBdaWrite(base, allocation) == 0,
            "an empty domain held a reader");
    scheduler.Finish();
    scheduler.DrainPriorityOperations();
    cache.ServiceDeferredBdaWrites();
    Require(name, "empty applied", !cache.HasPendingBdaWrites(), "the empty producer was not applied");
  });
  // The settled miss reaches the CPU through the production fault path.
  uint32_t miss_value = 0, value_c = 0;
  std::memcpy(&miss_value, reinterpret_cast<const void*>(page_b), sizeof(miss_value));
  std::memcpy(&value_c, reinterpret_cast<const void*>(page_c), sizeof(value_c));
  Require(name, "CPU values", miss_value == 0x66666666u && value_c == 0x44444444u,
          "the CPU read stale bytes of a settled page");
  OnGpuThread(context, [&] {
    scheduler.Finish();
    processor.reset();
  });
  Require(name, "unmap", LibKernel::Memory::KernelMunmap(base, allocation) == 0, "unmap failed");
  Require(name, "release", LibKernel::Memory::KernelReleaseDirectMemory(offset, allocation) == 0,
          "direct release failed");
  context.ShutdownGpu();
  LibKernel::Memory::InstallGpuResources(nullptr);
  std::printf("[host]    %-32s ok\n", name);
}
