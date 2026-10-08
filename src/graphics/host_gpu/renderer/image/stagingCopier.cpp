#include "graphics/host_gpu/renderer/image/stagingCopier.h"

#include "common/assert.h"
#include "common/hangWatchdog.h"
#include "common/hangTrace.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cinttypes>
#include <chrono>
#include <cstring>

namespace Libs::Graphics {

StagingCopier::StagingCopier(GraphicContext& graphics): m_graphics(graphics) {
	if (SubmitWaitBeforeSignal()) {
		vk::SemaphoreTypeCreateInfo type_info {};
		type_info.semaphoreType = vk::SemaphoreType::eTimeline;
		type_info.initialValue  = 0;
		vk::SemaphoreCreateInfo create_info {};
		create_info.pNext = &type_info;
		const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
		EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
	}
	m_range_pool.reserve(MaxRangeVectors);
	m_worker = std::jthread([this](std::stop_token stop) { Worker(stop); });
}

StagingCopier::~StagingCopier() {
	{
		std::scoped_lock lock(m_mutex);
		m_stopping = true;
	}
	m_available.notify_all();
	if (m_worker.joinable()) {
		m_worker.join();
	}
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

std::vector<StagingCopier::Range> StagingCopier::AcquireRanges(size_t minimum) {
	std::vector<Range> ranges;
	{
		std::scoped_lock lock(m_mutex);
		if (!m_range_pool.empty()) {
			ranges = std::move(m_range_pool.back());
			m_range_pool.pop_back();
		}
	}
	if (ranges.capacity() != 0) {
		Profiler::CountFrameEvent(Profiler::FrameEvent::CoherenceCopyRangeReuses);
		HangTrace::CountMemory(HangTrace::MemoryCounter::CoherenceRangeReuses);
	}
	if (ranges.capacity() < minimum) {
		ranges.reserve(minimum);
		Profiler::CountFrameEvent(Profiler::FrameEvent::CoherenceCopyRangeAllocations);
		HangTrace::CountMemory(HangTrace::MemoryCounter::CoherenceRangeAllocations);
	}
	return ranges;
}

void StagingCopier::Enqueue(std::vector<Range> ranges, Buffer* flush_buffer, uint64_t flush_offset,
                            uint64_t flush_size, std::shared_ptr<Verification> verification) {
	if (ranges.empty()) {
		return;
	}
	const bool coherence = ranges.front().backing_source != nullptr;
	uint64_t bytes = 0;
	for (const auto& range: ranges) {
		EXIT_IF((range.backing_source != nullptr) != coherence || range.size == 0);
		bytes += range.size;
	}
	if (coherence) {
		// Publish all sources before making the job available. One map lock per job.
		m_sources.Track(std::span<const Range>(ranges), m_enqueued + 1, [](const Range& range) {
			return reinterpret_cast<uint64_t>(range.backing_source);
		});
	}
	Profiler::CountFrameEvent(coherence ? Profiler::FrameEvent::CoherenceCopyJobs
	                                   : Profiler::FrameEvent::TextureAsyncCopies);
	Profiler::CountFrameEvent(coherence ? Profiler::FrameEvent::CoherenceCopyBytes
	                                   : Profiler::FrameEvent::TextureAsyncCopyBytes, bytes);
	if (coherence) {
		HangTrace::CountMemory(HangTrace::MemoryCounter::CoherenceCopyJobs);
		HangTrace::CountMemory(HangTrace::MemoryCounter::CoherenceCopyBytes, bytes);
	}
	bool wake;
	{
		std::scoped_lock lock(m_mutex);
		EXIT_IF(m_stopping);
		wake = m_waiting && m_jobs.empty();
		m_jobs.push_back({std::move(ranges), flush_buffer, flush_offset, flush_size, ++m_enqueued,
		                  std::move(verification)});
	}
	if (wake) {
		m_available.notify_one();
	}
}

uint64_t StagingCopier::PendingSourceValue(uint64_t address, uint64_t size) const {
	// Most writes see an idle copier: avoid even looking up the guest's backing in that case.
	if (size == 0 || m_sources.IsIdle()) {
		return 0;
	}
	const bool trace = HangTrace::Enabled();
	const auto start = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
	Profiler::ScopedFrameWait timing(Profiler::FrameWait::CoherenceCopyGuardLookup);
	Profiler::CountFrameEvent(Profiler::FrameEvent::CoherenceCopyGuardQueries);
	const auto value = [&] {
		EXIT_IF(UINT64_MAX - address < size);
		if (const auto* source = LibKernel::Memory::GuestBackingAlias(address, size); source != nullptr) {
			return m_sources.PendingValue(reinterpret_cast<uint64_t>(source), size);
		}
		// A write may span guest mappings, including noncontiguous views of the same direct backing.
		// Guest maps are host-page aligned. Coalesce adjacent canonical aliases before querying the map.
		constexpr uint64_t Page = 4096;
		uint64_t value = 0, run = 0, run_size = 0;
		for (uint64_t done = 0; done < size;) {
			const auto current = address + done;
			const auto bytes = std::min(size - done, Page - (current & (Page - 1)));
			const auto alias = reinterpret_cast<uint64_t>(LibKernel::Memory::GuestBackingAlias(current, bytes));
			if (run_size != 0 && (alias == 0 || alias != run + run_size)) {
				value = std::max(value, m_sources.PendingValue(run, run_size));
				run_size = 0;
			}
			if (alias != 0) {
				if (run_size == 0) {
					run = alias;
				}
				run_size += bytes;
			}
			done += bytes;
		}
		return run_size == 0 ? value : std::max(value, m_sources.PendingValue(run, run_size));
	}();
	if (trace) {
		const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
		    std::chrono::steady_clock::now() - start).count();
		HangTrace::CountMemory(HangTrace::MemoryCounter::CoherenceGuardQueries);
		HangTrace::CountMemory(HangTrace::MemoryCounter::CoherenceGuardLookupNs, ns);
	}
	return value;
}

void StagingCopier::BeforeEmulatorWrite(uint64_t address, uint64_t size) {
	WaitSource(PendingSourceValue(address, size));
}

void StagingCopier::WaitSource(uint64_t value) {
	if (value != 0) {
		const bool trace = HangTrace::Enabled();
		const auto start = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
		Profiler::CountFrameEvent(Profiler::FrameEvent::CoherenceCopyGuardWaits);
		Profiler::ScopedFrameWait timing(Profiler::FrameWait::CoherenceCopyGuard);
		WaitHost(value);
		if (trace) {
			const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
			    std::chrono::steady_clock::now() - start).count();
			HangTrace::CountMemory(HangTrace::MemoryCounter::CoherenceGuardWaits);
			HangTrace::CountMemory(HangTrace::MemoryCounter::CoherenceGuardNs, ns);
		}
	}
}

void StagingCopier::HoldWorkerForTest(bool hold) {
	{
		std::scoped_lock lock(m_mutex);
		m_hold = hold;
	}
	m_available.notify_one();
}

void StagingCopier::HoldWorkerAfterForTest(uint64_t value) {
	{
		std::scoped_lock lock(m_mutex);
		m_hold_after = value;
	}
	m_available.notify_one();
}

uint64_t StagingCopier::PendingValue() {
	// Acquire: when the worker has finished, its host writes happen-before the caller's
	// submission, which makes them available to the device (host write ordering guarantee).
	return m_completed.load(std::memory_order_acquire) >= m_enqueued ? 0 : m_enqueued;
}

void StagingCopier::WaitHost(uint64_t value) {
	auto completed = m_completed.load(std::memory_order_acquire);
	if (completed >= value) {
		return;
	}
	HangWatchdog::Scope scope("host-staging-copy", reinterpret_cast<uint64_t>(this), value,
	                          completed);
	while (completed < value) {
		HangTrace::SyncWait sync_wait("host-staging", "host-staging-copy", reinterpret_cast<uint64_t>(this), value);
		m_completed.wait(completed, std::memory_order_acquire);
		completed = m_completed.load(std::memory_order_acquire);
	}
}

void StagingCopier::Run(Job& job) {
	const bool coherence = job.ranges.front().backing_source != nullptr;
	const bool trace = coherence && HangTrace::Enabled();
	const auto start = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
	Profiler::ScopedFrameWait timing(coherence ? Profiler::FrameWait::CoherenceStagingCopy
	                                          : Profiler::FrameWait::TextureStagingCopy);
	uint64_t verify_offset = 0;
	// Short slices: TryReadBacking holds the guest address-space lock while it copies.
	constexpr uint64_t Slice = 256ull * 1024;
	for (const auto& range: job.ranges) {
		if (range.backing_source != nullptr) {
			const auto* source = range.backing_source;
			if (job.verification) {
				auto* observed = job.verification->observed.data() + verify_offset;
				std::memcpy(observed, source, range.size);
				source = observed;
				verify_offset += range.size;
			}
			std::memcpy(range.destination, source, range.size);
			continue;
		}
		for (uint64_t done = 0; done < range.size;) {
			const auto bytes = std::min(Slice, range.size - done);
			auto*      dst   = range.destination + done;
			const auto src   = range.guest_address + done;
			if (!LibKernel::Memory::TryReadBacking(src, dst, bytes) &&
			    !LibKernel::Memory::TryReadPrtBacking(src, dst, bytes)) {
				// The range was mapped when the refresh was recorded; unmapping drains the GPU
				// (and so this job) first. Never leave a submission waiting on a failed copy.
				std::memset(dst, 0, bytes);
				if (++m_read_failures <= 16) {
					LOGF("StagingCopier: failed to read guest image backing 0x%016" PRIx64
					     " size=0x%" PRIx64 "\n",
					     src, bytes);
				}
			}
			done += bytes;
		}
	}
	if (job.verification) {
		const auto& verify = *job.verification;
		Profiler::CountFrameEvent(Profiler::FrameEvent::CoherenceCopyVerifySourceChecks);
		HangTrace::CountMemory(HangTrace::MemoryCounter::CoherenceSourceChecks);
		if (verify.expected != verify.observed) {
			const auto epoch = verify.tracker->FaultMutationEpoch();
			if (epoch == UINT64_MAX || epoch != verify.fault_epoch) {
				Profiler::CountFrameEvent(Profiler::FrameEvent::CoherenceCopyVerifyRedirties);
				HangTrace::CountMemory(HangTrace::MemoryCounter::CoherenceSourceRedirties);
			} else {
				Profiler::CountFrameEvent(Profiler::FrameEvent::CoherenceCopyVerifyMismatches);
				HangTrace::CountMemory(HangTrace::MemoryCounter::CoherenceMismatches);
				LOGF("Coherence copy verify: source changed without a dirty mutation\n");
			}
		}
	}
	if (job.flush_buffer != nullptr && job.flush_size != 0) {
		job.flush_buffer->Flush(job.flush_offset, job.flush_size);
	}
	if (trace) {
		const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
		    std::chrono::steady_clock::now() - start).count();
		HangTrace::CountMemory(HangTrace::MemoryCounter::CoherenceCopyNs, ns);
	}
}

void StagingCopier::Worker(std::stop_token stop) {
	KYTY_PROFILER_THREAD("Host staging copies");
	(void)stop;
	for (;;) {
		Job job;
		{
			std::unique_lock lock(m_mutex);
			m_waiting = true;
			m_available.wait(lock, [this] {
				return m_stopping || (!m_hold && !m_jobs.empty() &&
				       (m_hold_after == 0 || m_completed.load(std::memory_order_relaxed) < m_hold_after));
			});
			m_waiting = false;
			if (m_jobs.empty()) {
				return; // stopping and drained
			}
			job = std::move(m_jobs.front());
			m_jobs.pop_front();
		}
		Run(job);
		if (job.ranges.front().backing_source != nullptr &&
		    job.ranges.capacity() <= MaxRetainedRanges) {
			job.ranges.clear();
			std::scoped_lock lock(m_mutex);
			if (m_range_pool.size() < MaxRangeVectors) {
				m_range_pool.push_back(std::move(job.ranges));
			}
		}
		m_completed.store(job.value, std::memory_order_release);
		m_completed.notify_all();
		if (m_semaphore != nullptr) {
			// KYTY_SUBMIT_WAIT_BEFORE_SIGNAL=1 only: batches already queued may wait for it.
			vk::SemaphoreSignalInfo signal {};
			signal.semaphore = m_semaphore;
			signal.value     = job.value;
			const auto result = m_graphics.device.signalSemaphore(&signal);
			EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
		}
	}
}

} // namespace Libs::Graphics
