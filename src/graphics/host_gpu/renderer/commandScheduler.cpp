#include "graphics/host_gpu/renderer/commandScheduler.h"

#include "common/assert.h"
#include "common/cpuPlacement.h"
#include "common/debugCounters.h"
#include "common/hangTrace.h"
#include "common/hangWatchdog.h"
#include "common/liveSwitch.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/rendererBatch.h"
#include "common/threads.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandPoolReuse.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/drawPrep/readSet.h"
#include "graphics/host_gpu/renderer/eopTimestamps.h"
#include "graphics/host_gpu/renderer/gpuOpProfiler.h"
#include "graphics/host_gpu/renderer/gpuTiming.h"
#include "graphics/host_gpu/watchdogSubmit.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace Libs::Graphics {

static thread_local CommandScheduler* g_deferred_callback_scheduler = nullptr;

namespace {

void ReportVulkanFatal(const char* what, vk::Result result, uint64_t tick, uint32_t debug_op,
                       uint64_t debug_submit, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                       uint32_t arg3, uint64_t arg4) {
	LOGF("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	     debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::printf("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	            what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	            debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::fflush(stdout);
}

} // namespace

CommandScheduler::CommandPool::CommandPool(GraphicContext& graphics, MasterSemaphore& master)
    : m_graphics(graphics), m_master(master) {
	EXIT_IF(graphics.queue_family == static_cast<uint32_t>(-1));
	vk::CommandPoolCreateInfo create {};
	create.queueFamilyIndex = graphics.queue_family;
	create.flags            = vk::CommandPoolCreateFlagBits::eTransient |
	                          vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	const auto result       = graphics.device.createCommandPool(&create, nullptr, &m_pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_pool == nullptr);
}

CommandScheduler::CommandPool::~CommandPool() {
	m_graphics.device.destroyCommandPool(m_pool, nullptr);
}

size_t CommandScheduler::CommandPool::Grow() {
	const auto first = m_ticks.size();
	m_ticks.resize(first + GrowStep);
	m_buffers.resize(first + GrowStep);

	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = m_pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = static_cast<uint32_t>(GrowStep);
	EXIT_IF(m_graphics.device.allocateCommandBuffers(&allocate, m_buffers.data() + first) !=
	        vk::Result::eSuccess);
	return first;
}

vk::CommandBuffer CommandScheduler::CommandPool::Commit() {
	// Reuse any slot already known to be retired before querying the driver's timeline.
	auto found = FindReusableCommandSlot(m_ticks, m_hint, m_master.KnownGpuTick(), [this] {
		m_master.Refresh();
		return m_master.KnownGpuTick();
	});
	if (!found) {
		if (m_recorder != nullptr) {
			// vkAllocateCommandBuffers needs the pool the recorder may be recording from.
			m_recorder->Drain(nullptr, false);
		}
		found = Grow();
	}

	m_ticks[*found] = m_master.CurrentTick();
	m_hint = (*found + 1) % m_ticks.size();
	return m_buffers[*found];
}

bool CommandScheduler::InDeferredOperation() noexcept {
	return g_deferred_callback_scheduler != nullptr;
}

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics, Role role)
    : m_master(graphics,
               role == Role::Guest && CommandRecorder::ConfiguredMode() != CommandRecorder::Mode::Off),
      m_context(context), m_graphics(graphics),
      m_command_pool(graphics, m_master), m_command(*this),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {
	if (role == Role::Guest && GpuTiming::Enabled()) {
		m_gpu_timing = std::make_unique<GpuTimestampRing>(graphics, GpuTimestampRing::GuestPairs);
		if (!m_gpu_timing->Valid()) {
			m_gpu_timing.reset();
		}
	}
	if (role == Role::Guest && EopTimestamps::GpuEnabled()) {
		m_eop_timestamps = std::make_unique<EopTimestampRing>(graphics);
		if (!m_eop_timestamps->Valid()) {
			m_eop_timestamps.reset();
		}
	}
	m_gpu_ops = role == Role::Guest && GpuOpProfiler::Enabled();
	if (const auto mode = CommandRecorder::ConfiguredMode();
	    role == Role::Guest && mode != CommandRecorder::Mode::Off) {
		m_recorder = std::make_unique<CommandRecorder>(graphics, m_master, m_gpu_timing.get(),
		                                               m_gpu_ops, mode);
		m_command.m_recorder = m_recorder.get();
		m_command.m_encoder  = &m_recorder->Encoder();
		m_command_pool.SetRecorder(m_recorder.get());
	}
}

CommandScheduler::~CommandScheduler() {
	Shutdown();
}

void CommandScheduler::Shutdown() {
	{
		std::unique_lock lock(m_operation_mutex);
		if (m_operation_state == OperationState::Closed) {
			return;
		}
		if (g_deferred_callback_scheduler == this) {
			EXIT_IF(m_operation_state == OperationState::Open);
			// A priority callback cannot join its own runner, while a normal callback can be
			// executing inside the shutdown owner's final PopPendingOperations. The owning
			// thread will finish shutdown after this callback returns.
			return;
		}
		if (m_operation_state == OperationState::Draining) {
			m_operation_available.wait(
			    lock, [this] { return m_operation_state == OperationState::Closed; });
			return;
		}
		m_operation_state = OperationState::Draining;
	}
	if (!m_command.IsInvalid()) {
		Submit({}, true);
	}
	m_master.Wait(CurrentTick() - 1);
	if (m_recorder != nullptr) {
		// Every tick was recorded and handed to the queue: stop the recorder thread before the
		// timing ring (its single producer until now) and the pool are used from here.
		m_recorder->Stop();
	}
	if (m_gpu_timing) {
		// Every submitted tick is complete: collect the remaining pairs before the ring (and its
		// query pool) is destroyed with this scheduler. No slot is left recording here.
		m_gpu_timing->Collect(m_master.KnownGpuTick(), true);
	}
	if (m_gpu_ops) {
		GpuOpProfiler::OnSchedulerShutdown(m_graphics);
	}
	PopPendingOperations();
	DrainPriorityOperations();
	m_priority_thread.request_stop();
	m_operation_available.notify_all();
	{
		// The runner re-checks its stop token only under the lock it sleeps with.
		std::lock_guard lock(m_operation_mutex);
	}
	m_priority_available.notify_all();
	if (m_priority_thread.joinable()) {
		m_priority_thread.join();
	}
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(!m_pending_operations.empty() || !m_priority_operations.empty() ||
		        m_priority_active);
		m_operation_state = OperationState::Closed;
	}
	m_operation_available.notify_all();
}

void CommandScheduler::Begin(HW::Context& registers, HW::UserConfig& user_config,
                             HW::Shader& shaders) {
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(m_operation_state != OperationState::Open);
	}
	m_command.Bind(registers, user_config, shaders);

	if (m_command.IsInvalid()) {
		BeginNext();
	}
}

void CommandScheduler::BeginRendering(const RenderState& state) {
	Current().BeginRendering(state);
}

void CommandScheduler::EndRendering() {
	if (Active() && !m_command.IsInvalid()) {
		Current().EndRendering();
	}
}

void CommandScheduler::Flush() {
	SubmitInfo submit;
	Flush(submit);
}

void CommandScheduler::Flush(SubmitInfo& submit) {
	Submit(submit);
	BeginNext();
}

void CommandScheduler::FlushAndWait() {
	HangTrace::SyncWait sync_wait("scheduler-finish", "flush-and-wait", reinterpret_cast<uint64_t>(this), CurrentTick());
	KYTY_PROFILER_DETAIL_FUNCTION();
	const auto tick = Submit({}, true);
	m_master.Wait(tick);
	BeginNext();
}

void CommandScheduler::Finish() {
	HangTrace::SyncWait sync_wait("scheduler-finish", "finish", reinterpret_cast<uint64_t>(this), CurrentTick());
	KYTY_PROFILER_DETAIL_FUNCTION();
	CheckActive();
	if (!m_command.IsInvalid()) {
		Submit({}, true);
	}
	m_master.Wait(CurrentTick() - 1);
	BeginNext();
	PopPendingOperations();
}

void CommandScheduler::FlushAndWaitPriorityPublication(std::source_location caller) {
	CheckActive();
	EXIT_IF(g_deferred_callback_scheduler == this);
	const auto tick = CurrentTick();
	HangTrace::SyncWait dependency("publication-dependency", "readback-publication",
	                               reinterpret_cast<uint64_t>(this), tick, caller);
	bool has_publication;
	{
		std::lock_guard lock(m_operation_mutex);
		has_publication = (m_priority_active && m_priority_active_tick == tick) ||
		                  (!m_priority_operations.empty() && m_priority_operations.back().tick == tick);
	}
	const auto submitted = Submit({}, true);
	EXIT_IF(submitted != tick);
	if (!has_publication) {
		// This entry point is also safe for a recording with no priority callback.
		m_master.Wait(tick, caller);
	}
	std::optional<Profiler::ScopedFrameWait> frame_wait;
	if (has_publication && GuestGpu::IsGpuThread() && Profiler::AggregateEnabled()) {
		frame_wait.emplace(Profiler::CurrentGpuWaitReason());
	}
	WaitPriorityOperations(tick, caller);
	frame_wait.reset();
	BeginNext();
}

void CommandScheduler::Wait(uint64_t tick, std::source_location caller) {
	HangTrace::SyncWait scheduler_wait("scheduler", "resource-dependency", reinterpret_cast<uint64_t>(this), tick, caller);
	EXIT_IF(tick > CurrentTick());
	if (tick == CurrentTick()) {
		KYTY_PROFILER_DETAIL_BLOCK("CommandScheduler::Wait (forced submit-then-wait)");
		CheckActive();
		// A stream-buffer wrap can wait while a draw is being prepared through a reference to
		// Current(). The wrapper stays stable while its pooled Vulkan buffer is retired. Deferred
		// resources are released only at the next GPU operation boundary.
		const auto submitted_tick = Submit({}, true);
		EXIT_IF(submitted_tick != tick);
		m_master.Wait(tick, caller);
		BeginNext();
	} else {
		m_master.Wait(tick, caller);
	}
}

void CommandScheduler::PopPendingOperations() {
	PopOperations(true);
}

// KYTY_PENDING_OPS_NOWAIT (default on; =0 waits as PopPendingOperations does). Live switch
// (common/liveSwitch.h): each call picks one of two valid behaviours.
static Live::Switch g_pending_ops_nowait("KYTY_PENDING_OPS_NOWAIT", Live::ParseDefaultOn);

static bool PendingOpsNoWait() {
	return g_pending_ops_nowait.On();
}

void CommandScheduler::PopReadyOperations() {
	PopOperations(!PendingOpsNoWait());
}

bool CommandScheduler::PriorityDoneLocked(uint64_t tick) const noexcept {
	const bool active_before_or_at = m_priority_active && m_priority_active_tick <= tick;
	const bool queued_before_or_at =
	    !m_priority_operations.empty() && m_priority_operations.front().tick <= tick;
	const bool collection_before_or_at =
	    (m_collection_active && m_collection_active_tick <= tick) ||
	    (!m_collection_operations.empty() && m_collection_operations.front().tick <= tick);
	return !active_before_or_at && !queued_before_or_at && !collection_before_or_at &&
	       CoherenceDoneLocked(tick);
}


void CommandScheduler::SetCoherenceHooks(AppliedHook applied, ServiceHook service, void* context) {
	std::lock_guard lock(m_operation_mutex);
	EXIT_IF((applied == nullptr) != (service == nullptr));
	m_coherence_applied = applied;
	m_coherence_service = service;
	m_coherence_context = context;
	m_coherence_owner = std::this_thread::get_id();
}

void CommandScheduler::ServiceCoherence() {
	if (m_coherence_service == nullptr || m_servicing_coherence ||
	    m_coherence_owner != std::this_thread::get_id()) return;
	m_servicing_coherence = true;
	m_coherence_service(m_coherence_context);
	m_servicing_coherence = false;
}

void CommandScheduler::SetCoherencePrefix(uint64_t ticket) {
	std::lock_guard lock(m_operation_mutex);
	EXIT_IF(m_coherence_applied == nullptr || ticket < m_coherence_prefix);
	m_coherence_prefix = ticket;
	const auto applied = m_coherence_applied(m_coherence_context);
	std::erase_if(m_coherence_frontiers, [applied](const auto& frontier) {
		return frontier.prefix <= applied;
	});
	if (!m_coherence_frontiers.empty() && m_coherence_frontiers.back().tick == CurrentTick())
		m_coherence_frontiers.back().prefix = ticket;
	else
		m_coherence_frontiers.push_back({CurrentTick(), ticket});
}

void CommandScheduler::NotifyCoherenceApplied() {
	m_operation_available.notify_all();
	m_priority_available.notify_one();
}

bool CommandScheduler::CoherenceDoneLocked(uint64_t tick) const noexcept {
	if (m_coherence_applied == nullptr) return true;
	const auto applied = m_coherence_applied(m_coherence_context);
	for (const auto& frontier : m_coherence_frontiers)
		if (frontier.tick <= tick && frontier.prefix > applied) return false;
	return true;
}

bool CommandScheduler::PublicationReadyLocked() const noexcept {
	return !m_priority_operations.empty() &&
	       (m_coherence_applied == nullptr ||
	        m_priority_operations.front().coherence_prefix <=
	            m_coherence_applied(m_coherence_context));
}

void CommandScheduler::DeferCollectionOperation(Common::UniqueFunction<void>&& operation,
                                                uint64_t tick) {
	CheckActive();
	EXIT_IF(!operation || tick != CurrentTick());
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(m_operation_state != OperationState::Open);
		m_preserve_current_completion = true;
		m_collection_operations.push({std::move(operation), tick});
	}
	m_priority_available.notify_one();
}

// KYTY_PENDING_REFRESH_US=<n> (default 0: off; BryanKAdams/KytyPS5 e4a7551): the non-waiting pop
// at draw entry (PopReadyOperations) queries the GPU's progress (vkGetSemaphoreCounterValue) at
// most once per n microseconds; between queries it runs what the last known progress allows. The
// operations are deletions, recycling and fault-buffer processing that nothing waits for; every
// blocking pop, explicit wait and allocation path queries as before. Live switch
// (common/liveSwitch.h): a parameter read at every pop.
static Live::Switch g_pending_refresh_us("KYTY_PENDING_REFRESH_US", [](const char* value) -> int64_t {
	return value == nullptr ? 0 : static_cast<int64_t>(std::strtoull(value, nullptr, 10));
});

static uint64_t PendingRefreshIntervalNs() {
	return static_cast<uint64_t>(g_pending_refresh_us.Get()) * 1000u;
}

void CommandScheduler::PopOperations(bool wait_for_priority) {
	ServiceCoherence();
	if (Common::RendererBatchEnabled()) {
		uint64_t first_tick = 0;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty()) return;
			first_tick = m_pending_operations.front().tick;
		}
		// A callback on the recording tick cannot have completed. Known completed
		// ticks need no driver query; explicit waits and allocation paths still refresh.
		if (first_tick >= CurrentTick()) return;
		if (!m_master.IsFree(first_tick)) {
			if (const auto interval = PendingRefreshIntervalNs(); interval != 0 && !wait_for_priority) {
				// The operations are queued in tick order: with the first one not known complete,
				// none is, until the next query.
				const auto now = GpuTiming::NowNs();
				if (now - m_last_pending_refresh_ns < interval) return;
				m_last_pending_refresh_ns = now;
			}
			m_master.Refresh();
		}
	} else {
		m_master.Refresh();
	}
	for (;;) {
		PendingOperation operation;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty() ||
			    !m_master.IsFree(m_pending_operations.front().tick)) {
				return;
			}
			if (!wait_for_priority && !PriorityDoneLocked(m_pending_operations.front().tick)) {
				// The runner has not finished this tick's priority operations. Leave this operation
				// and the ones after it queued, in order: a later pop runs them, and every blocking
				// pop (Finish, the fault manager) waits as before.
				Profiler::CountFrameEvent(Profiler::FrameEvent::PendingOpsDeferred);
				Profiler::CountFrameEvent(Profiler::FrameEvent::PendingOpsDeferredDepth,
				                          m_pending_operations.size());
				HangTrace::NotePendingOperations(m_pending_operations.size());
				return;
			}
			operation = std::move(m_pending_operations.front());
			m_pending_operations.pop();
		}
		if (wait_for_priority) {
			WaitPriorityOperations(operation.tick);
		}
		// Without waiting, the priority operations of this tick and earlier are done: new ones
		// carry the recording tick, which is later than any completed one.
		RunOperation(std::move(operation.callback));
	}
}

void CommandScheduler::DeferOperation(Common::UniqueFunction<void>&& operation) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		if (m_graphics.submission_queue.Enabled()) {
			m_preserve_current_completion = true;
		}
		if (Profiler::AggregateEnabled()) {
			m_diagnostic_generic_completion = true;
		}
		m_pending_operations.push({std::move(operation), CurrentTick()});
		return;
	}
	if (g_deferred_callback_scheduler == this) {
		lock.unlock();
		operation();
		return;
	}
	m_operation_available.wait(lock,
	                           [this] { return m_operation_state == OperationState::Closed; });
	lock.unlock();
	operation();
}

void CommandScheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& operation,
                                             PriorityOperationKind kind,
                                             std::source_location caller) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		if (m_graphics.submission_queue.Enabled()) {
			m_preserve_current_completion = true;
		}
		if (Profiler::AggregateEnabled()) {
			if (kind == PriorityOperationKind::EopInterrupt) {
				m_diagnostic_eop_completion = true;
			} else {
				m_diagnostic_generic_completion = true;
			}
		}
		const bool was_empty = m_priority_operations.empty();
		m_priority_operations.push({std::move(operation), CurrentTick(), caller, kind,
		                            HangTrace::g_sync_resource.address, HangTrace::g_sync_resource.size,
		                            m_coherence_prefix});
		lock.unlock();
		if (!PriorityWakeupsBatched()) {
			m_operation_available.notify_one();
			m_priority_available.notify_one();
		} else if (was_empty) {
			// The runner sleeps only on an empty queue; otherwise it pops this one by itself.
			m_priority_available.notify_one();
		}
		return;
	}
	if (g_deferred_callback_scheduler == this) {
		lock.unlock();
		operation();
		return;
	}
	m_operation_available.wait(lock,
	                           [this] { return m_operation_state == OperationState::Closed; });
	lock.unlock();
	operation();
}

bool CommandScheduler::PriorityWakeupsBatched() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_PRIORITY_WAKE_BATCH");
		return value == nullptr || std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

void CommandScheduler::SetProgressHook(ProgressHook hook, void* context) {
	std::lock_guard lock(m_operation_mutex);
	m_progress_hook         = hook;
	m_progress_hook_context = context;
}

void CommandScheduler::PriorityOperationsThread(std::stop_token stop) {
	KYTY_PROFILER_THREAD("GPU completion");
	Common::RaiseServiceThreadPriority();
	uint32_t placement_count = 0;
	while (!stop.stop_requested()) {
		PendingOperation operation;
		ProgressHook hook = nullptr;
		void* hook_context = nullptr;
		bool collection = false;
		{
			std::unique_lock lock(m_operation_mutex);
			m_priority_available.wait(lock, [this, &stop] {
				return stop.stop_requested() || !m_collection_operations.empty() ||
				       PublicationReadyLocked();
			});
			if (stop.stop_requested()) return;
			if (!m_collection_operations.empty() &&
			    !m_master.IsFree(m_collection_operations.front().tick)) {
				lock.unlock();
				m_master.Refresh();
				lock.lock();
				if (!PublicationReadyLocked() &&
				    !m_master.IsFree(m_collection_operations.front().tick)) {
					m_priority_available.wait_for(lock, std::chrono::microseconds(250));
					continue;
				}
			}
			// A gated label never takes the runner. Collections can therefore advance
			// and wake their GPU owner without a dependency on CPU application.
			collection = !PublicationReadyLocked() ||
			    (!m_collection_operations.empty() && m_master.IsFree(m_collection_operations.front().tick) &&
			     m_collection_operations.front().tick <= m_priority_operations.front().tick);
			auto& queue = collection ? m_collection_operations : m_priority_operations;
			operation = std::move(queue.front());
			queue.pop();
			if (collection) {
				m_collection_active = true;
				m_collection_active_tick = operation.tick;
			} else {
				m_priority_active = true;
				m_priority_active_tick = operation.tick;
			}
			hook = m_progress_hook;
			hook_context = m_progress_hook_context;
		}
		HangTrace::SyncResource resource(operation.trace_address, operation.trace_size);
		m_master.Wait(operation.tick, operation.caller,
		              collection ? "coherence-collection" :
		              operation.kind == PriorityOperationKind::EopInterrupt
		                  ? "eop-interrupt" : "resource-publication");
		RunOperation(std::move(operation.callback));
		Profiler::CountFrameEvent(Profiler::FrameEvent::PriorityOperationsRun);
		if ((++placement_count & 15u) == 0u)
			Common::SamplePlacement(Common::ThreadRole::Host);
		if (hook != nullptr) hook(hook_context);
		{
			std::lock_guard lock(m_operation_mutex);
			if (collection) {
				m_collection_active = false;
				m_collection_active_tick = 0;
			} else {
				m_priority_active = false;
				m_priority_active_tick = 0;
			}
			m_priority_progress.fetch_add(1, std::memory_order_release);
			const bool tick_done =
			    (m_priority_operations.empty() || m_priority_operations.front().tick != operation.tick) &&
			    (m_collection_operations.empty() || m_collection_operations.front().tick != operation.tick);
			if (!PriorityWakeupsBatched() || (m_priority_waiters != 0 && tick_done)) {
				m_operation_available.notify_all();
				if (PriorityWakeupsBatched())
					Profiler::CountFrameEvent(Profiler::FrameEvent::PriorityWaiterWakeups);
			}
		}
	}
}

void CommandScheduler::DrainPriorityOperations() {
	HangWatchdog::Scope wait("priority-drain", reinterpret_cast<uint64_t>(this));
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	++m_priority_waiters;
	for (;;) {
		lock.unlock();
		ServiceCoherence();
		NotifyCoherenceApplied();
		lock.lock();
		if (m_priority_operations.empty() && !m_priority_active &&
		    m_collection_operations.empty() && !m_collection_active &&
		    CoherenceDoneLocked(UINT64_MAX)) break;
		if (m_coherence_service != nullptr && m_coherence_owner == std::this_thread::get_id())
			m_operation_available.wait_for(lock, std::chrono::microseconds(250));
		else
			m_operation_available.wait(lock);
	}
	--m_priority_waiters;
}

// KYTY_PRIORITY_WAIT_SPIN_US (default 0): WaitPriorityOperations spins this long before blocking.
// Live switch (common/liveSwitch.h), read at each wait.
static Live::Switch g_priority_wait_spin_us("KYTY_PRIORITY_WAIT_SPIN_US", [](const char* value) -> int64_t {
	const auto us = value != nullptr ? std::strtoull(value, nullptr, 10) : 0ull;
	return static_cast<int64_t>(std::min<unsigned long long>(us, 100'000ull));
});

static uint64_t PriorityWaitSpinNs() {
	return static_cast<uint64_t>(g_priority_wait_spin_us.Get()) * 1000u;
}

static void PriorityWaitRelax() {
#if defined(_M_X64) || defined(__x86_64__)
	_mm_pause();
#else
	std::this_thread::yield();
#endif
}

void CommandScheduler::WaitPriorityOperations(uint64_t tick, std::source_location caller) {
	ServiceCoherence();
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	if (PriorityDoneLocked(tick)) {
		return;
	}
	HangTrace::SyncWait priority_wait("priority-completion", "host-publication", reinterpret_cast<uint64_t>(this), tick, caller);
	HangWatchdog::Scope wait("priority-completion", reinterpret_cast<uint64_t>(this), tick,
	                         m_priority_active_tick, 0, m_priority_operations.size());
	if (const auto spin_ns = PriorityWaitSpinNs(); spin_ns != 0) {
		// Spin on the runner's progress counter and take the lock only when it moves: the
		// runner needs the lock to finish each operation.
		Profiler::CountFrameEvent(Profiler::FrameEvent::PriorityWaitSpins);
		auto       seen  = m_priority_progress.load(std::memory_order_acquire);
		const auto start = GpuTiming::NowNs();
		lock.unlock();
		for (uint32_t spins = 1;; spins++) {
			PriorityWaitRelax();
			if (const auto progress = m_priority_progress.load(std::memory_order_acquire);
			    progress != seen) {
				seen = progress;
				lock.lock();
				if (PriorityDoneLocked(tick)) {
					Profiler::CountFrameEvent(Profiler::FrameEvent::PriorityWaitSpinHits);
					return;
				}
				lock.unlock();
			}
			if ((spins & 63u) == 0u && GpuTiming::NowNs() - start > spin_ns) {
				break;
			}
		}
		lock.lock();
	}
	++m_priority_waiters;
	const auto publication_wait_begin = std::chrono::steady_clock::now();
	while (!PriorityDoneLocked(tick)) {
		if (m_coherence_service != nullptr && m_coherence_owner == std::this_thread::get_id()) {
			lock.unlock();
			ServiceCoherence();
			NotifyCoherenceApplied();
			lock.lock();
			if (!PriorityDoneLocked(tick))
				m_operation_available.wait_for(lock, std::chrono::microseconds(250));
		} else {
			m_operation_available.wait(lock);
		}
	}
	--m_priority_waiters;
	Common::DebugCounters::Add(Common::DebugCounters::Counter::GpuPublicationWaits);
	const auto publication_wait_ns = static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(
	        std::chrono::steady_clock::now() - publication_wait_begin).count());
	Common::DebugCounters::Add(
	    Common::DebugCounters::Counter::GpuPublicationWaitNs, publication_wait_ns);
	if (GuestGpu::IsGpuThread()) {
		Common::DebugCounters::Add(Common::DebugCounters::Counter::CpPublicationWaits);
		Common::DebugCounters::Add(Common::DebugCounters::Counter::CpPublicationWaitNs,
		                          publication_wait_ns);
	}
}

void CommandScheduler::RunOperation(Common::UniqueFunction<void>&& operation) {
	HangWatchdog::Scope callback("priority-or-pending-callback", reinterpret_cast<uint64_t>(this));
	auto* previous                = g_deferred_callback_scheduler;
	g_deferred_callback_scheduler = this;
	operation();
	g_deferred_callback_scheduler = previous;
}

bool CommandScheduler::IsFree(uint64_t tick) {
	if (m_master.IsFree(tick)) {
		return true;
	}
	m_master.Refresh();
	return m_master.IsFree(tick);
}

void CommandScheduler::CheckActive() const {
	EXIT_IF(!Active());
	// Draw-prep workers only prepare; recording belongs to the GPU thread.
	EXIT_IF(DrawPrep::IsWorkerThread());
}

CommandBuffer& CommandScheduler::Current() {
	CheckActive();
	return m_command;
}

CommandBuffer& CommandScheduler::BeginCommand() {
	EXIT_IF(!m_command.IsInvalid());
	m_command.m_buffer = m_command_pool.Commit();
	m_command.Begin();
	if (m_recorder != nullptr) {
		// The recorder begins the native buffer and stamps KYTY_GPU_TIMING / GpuOpProfiler there,
		// with this recording's tick and start time.
		m_recorder->Begin(m_command.m_buffer, m_master.CurrentTick(),
		                  m_gpu_timing ? GpuTiming::NowNs() : 0);
		if (m_eop_timestamps) {
			// KYTY_EOP_TIMESTAMPS=gpu: reset the slots whose results were read, as recorder
			// packets after the recorder's Begin (no drain). Without it no slot would ever become
			// free.
			m_eop_timestamps->BeginCommand(m_command.StateSink());
		}
		return m_command;
	}
	if (m_gpu_timing) {
		// Reuse the retirement point that just recycled a command buffer: read completed pairs
		// without waiting, then reset and stamp this buffer's pair before any rendering begins.
		m_gpu_timing->Collect(m_master.KnownGpuTick());
		m_gpu_timing->BeginCommand(m_command.m_buffer);
	}
	if (m_eop_timestamps) {
		// Outside rendering: reset the guest timestamp slots whose results were read.
		m_eop_timestamps->BeginCommand(m_command.StateSink());
	}
	if (m_gpu_ops) {
		GpuOpProfiler::OnBeginCommand(m_graphics, m_command.m_buffer, m_master.CurrentTick(),
		                              m_master.KnownGpuTick());
	}
	return m_command;
}

uint64_t CommandScheduler::Submit(SubmitInfo submit, bool force_completion) {
	HangWatchdog::NoteSubmission();
	EXIT_IF(m_command.IsInvalid());
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);
	if (m_pre_submit_hook != nullptr && !m_in_pre_submit && Active()) {
		// Work its owner deferred to the end of this command buffer (SetPreSubmitHook).
		m_in_pre_submit = true;
		m_pre_submit_hook(m_pre_submit_hook_context);
		m_in_pre_submit = false;
		EXIT_IF(m_command.IsInvalid());
	}
	for (auto* dependency: m_submit_dependencies) {
		if (dependency == nullptr) {
			continue;
		}
		// Staging bytes that recorded commands read may still be copied by a host worker (or, for
		// the upload DMA, by the transfer queue).
		if (const auto value = dependency->PendingValue(); value != 0) {
			if (!SubmitWaitBeforeSignal()) {
				// Waited for by the thread that submits the batch (SubmitDependency).
				submit.AddHostDependency(dependency, value);
			}
			const auto semaphore = dependency->Semaphore();
			if (semaphore == nullptr) {
				continue; // host work: done before vkQueueSubmit
			}
			if (submit.num_wait_semaphores < SubmitInfo::MaxSemaphores) {
				submit.AddWait(semaphore, value, dependency->WaitStages());
			} else {
				dependency->WaitHost(value);
			}
		}
	}
	const auto count_boundary = [this, &submit, force_completion] {
		if (!Profiler::AggregateEnabled()) {
			return;
		}
		// Explicit waits/signals independently prevent native submit coalescing. Include
		// them as generic obstacles so EopOnly describes the potential optimization scope.
		// The caller holds m_operation_mutex across this and the submit tick transition.
		const bool generic = m_diagnostic_generic_completion || force_completion ||
		                     submit.num_wait_semaphores != 0 || submit.num_signal_semaphores != 0;
		const auto kind = m_diagnostic_eop_completion
		                      ? (generic ? Profiler::FrameEvent::SubmitBoundaryEopMixed
		                                 : Profiler::FrameEvent::SubmitBoundaryEopOnly)
		                      : (generic ? Profiler::FrameEvent::SubmitBoundaryGeneric
		                                 : Profiler::FrameEvent::SubmitBoundaryUnprotected);
		Profiler::CountFrameEvent(kind);
		m_diagnostic_eop_completion     = false;
		m_diagnostic_generic_completion = false;
	};

	const uint64_t submit_ns = m_gpu_timing ? GpuTiming::NowNs() : 0;
	if (m_recorder != nullptr) {
		// KYTY_CP_RECORDER: the final render-pass end and barrier batch are encoded like every
		// other command (or recorded natively in an open direct window); the recorder then stamps
		// the timing end, ends the buffer and hands it to the broker or the queue. The tick is
		// allocated here, exactly as in queued mode.
		m_command.EndRendering();
		m_command.FlushBarriers();
		m_command.CloseDirectWindow();
		CommandStream::SubmitPacket packet;
		packet.submit       = submit;
		packet.submit_ns    = submit_ns;
		packet.debug_op     = m_command.m_debug_op;
		packet.debug_submit = m_command.m_debug_submit_id;
		packet.debug_arg0   = m_command.m_debug_arg0;
		packet.debug_arg1   = m_command.m_debug_arg1;
		packet.debug_arg2   = m_command.m_debug_arg2;
		packet.debug_arg3   = m_command.m_debug_arg3;
		packet.debug_arg4   = m_command.m_debug_arg4;
		{
			std::lock_guard lock(m_operation_mutex);
			count_boundary();
			packet.tick = m_master.NextTick();
			packet.submit.AddSignal(m_master.Handle(), packet.tick);
			packet.preserve_completion =
			    (force_completion || m_preserve_current_completion) ? 1u : 0u;
			m_preserve_current_completion = false;
		}
		m_recorder->Submit(packet);
		m_command.m_buffer = nullptr;
		return packet.tick;
	}
	{
		KYTY_PROFILER_DETAIL_BLOCK("CommandScheduler::SubmitEnd");
		if (m_gpu_timing) {
			// End's own EndRendering becomes a no-op; the end stamp follows the final store ops.
			m_command.EndRendering();
			// Pending batched barriers belong before the end stamp, as when recorded directly.
			m_command.FlushBarriers();
			m_gpu_timing->EndCommand(m_command.m_buffer);
		}
		m_command.End();
	}
	const auto buffer   = m_command.m_buffer;
	auto&      graphics = m_graphics;
	EXIT_IF(graphics.queue == nullptr);

	if (graphics.submission_queue.Enabled()) {
		KYTY_PROFILER_DETAIL_BLOCK("CommandScheduler::QueueDispatch");
		QueuedSubmission queued {.submit = submit,
		                         .progress = m_master.GetSubmissionProgress(),
		                         .master_semaphore = m_master.Handle(),
		                         .command = buffer,
		                         .debug_op = m_command.m_debug_op,
		                         .debug_submit = m_command.m_debug_submit_id,
		                         .debug_arg0 = m_command.m_debug_arg0,
		                         .debug_arg1 = m_command.m_debug_arg1,
		                         .debug_arg2 = m_command.m_debug_arg2,
		                         .debug_arg3 = m_command.m_debug_arg3,
		                         .debug_arg4 = m_command.m_debug_arg4};
		{
			// The callback runner may already have popped a callback. Record the
			// boundary at registration, never by inspecting the callback queues here.
			std::lock_guard lock(m_operation_mutex);
			count_boundary();
			queued.tick = m_master.NextTick();
			queued.submit.AddSignal(m_master.Handle(), queued.tick);
			queued.preserve_completion = force_completion || m_preserve_current_completion;
			m_preserve_current_completion = false;
		}
		const auto tick = queued.tick;
		if (m_gpu_timing) {
			// The broker stores the native submit time through this pointer before it publishes
			// the tick; the slot is not reused until KnownGpuTick() covers the tick.
			queued.dispatch_ns = m_gpu_timing->Submitted(tick, submit_ns);
		}
		graphics.submission_queue.Enqueue(std::move(queued));
		m_command.m_buffer = nullptr;
		return tick;
	}

	// Direct submission: this thread waits for the work the batch reads (SubmitDependency).
	submit.WaitHostDependencies();
	vk::Result result;
	uint64_t   tick;
	{
		KYTY_PROFILER_DETAIL_BLOCK("CommandScheduler::QueueDispatch");
		Common::LockGuard lock(graphics.queue_mutex);
		if (Profiler::AggregateEnabled()) {
			std::lock_guard operation_lock(m_operation_mutex);
			count_boundary();
			tick = m_master.NextTick();
		} else {
			tick = m_master.NextTick();
		}
		submit.AddSignal(m_master.Handle(), tick);

		vk::TimelineSemaphoreSubmitInfo timeline_info {};
		timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
		timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
		timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
		timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

		vk::SubmitInfo submit_info {};
		submit_info.pNext                = &timeline_info;
		submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
		submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
		submit_info.pWaitDstStageMask    = submit.wait_stages.data();
		submit_info.commandBufferCount   = 1;
		submit_info.pCommandBuffers      = &buffer;
		submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
		submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

		{
			KYTY_PROFILER_DETAIL_BLOCK("CommandScheduler::DriverSubmit");
			HangWatchdog::Scope native(
			    "vkQueueSubmit-direct",
			    reinterpret_cast<uint64_t>(static_cast<VkQueue>(graphics.queue)), tick, 0,
			    submit.num_wait_semaphores, submit.num_signal_semaphores);
			NoteWatchdogSubmit(graphics.queue, submit_info, tick);
			result = graphics.queue.submit(1, &submit_info, nullptr);
		}
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::QueueSubmits);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::QueueSubmitCommandBuffers);

	if (result == vk::Result::eErrorDeviceLost) {
		DumpDeviceLossDiagnostics(graphics, tick);
	}
	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkQueueSubmit", result, tick, m_command.m_debug_op,
		                  m_command.m_debug_submit_id, m_command.m_debug_arg0,
		                  m_command.m_debug_arg1, m_command.m_debug_arg2, m_command.m_debug_arg3,
		                  m_command.m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	if (m_gpu_timing) {
		if (auto* dispatch_ns = m_gpu_timing->Submitted(tick, submit_ns); dispatch_ns != nullptr) {
			*dispatch_ns = GpuTiming::NowNs();
		}
	}

	m_command.m_buffer = nullptr;
	return tick;
}

void CommandScheduler::WaitRecorded(uint64_t tick, bool from_producer) {
	if (m_recorder != nullptr && m_recorder->GetMode() == CommandRecorder::Mode::Thread) {
		m_recorder->WaitRecorded(tick, from_producer);
	}
}

void CommandScheduler::BeginNext() {
	KYTY_PROFILER_DETAIL_BLOCK("CommandScheduler::BeginNext");
	CheckActive();
	BeginCommand();
}

} // namespace Libs::Graphics
