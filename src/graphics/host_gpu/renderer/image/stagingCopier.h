#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_STAGINGCOPIER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_STAGINGCOPIER_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/sourceCopyTracker.h"
#include "graphics/host_gpu/pageManager.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class Buffer;
class MemoryTracker;

// Guest memory -> staging buffer copies for texture refreshes, made on a host worker after the
// commands reading the staging bytes were recorded (KYTY_TEXTURE_ASYNC_STAGING, default on).
// Every job publishes its value once its bytes are written and flushed. The thread that submits
// the guest scheduler's next batch waits for the latest value on the host before vkQueueSubmit
// (SubmitDependency), so the recording thread never waits for the copy, and the GPU never waits
// for a signal that only the host can make later. With KYTY_SUBMIT_WAIT_BEFORE_SIGNAL=1 the job
// signals a timeline semaphore instead and the batch waits for it on the GPU (up to int7).
//
// The source pages are protected before copying: by the image at admission, or by the F3
// prefix job on this same FIFO worker. A
// guest write racing with the copy faults and dirties its chunk (TextureCache), so the next
// refresh replaces whatever the copy saw of that chunk.
class StagingCopier final: public SubmitDependency {
public:
	struct Range {
		uint64_t guest_address = 0;
		uint8_t* destination   = nullptr;
		uint64_t size          = 0;
		const uint8_t* backing_source = nullptr; // stable direct alias, buffer uploads only
	};

	// Verify uses ordinary host memory; mapped upload memory may be write-combined.
	struct Verification {
		const MemoryTracker* tracker = nullptr;
		uint64_t fault_epoch = 0;
		std::vector<uint8_t> expected; // source at admission
		std::vector<uint8_t> observed; // source copied by worker, reference for GPU readback
	};

	explicit StagingCopier(GraphicContext& graphics);
	~StagingCopier() override;
	KYTY_CLASS_NO_COPY(StagingCopier);

	// Recording thread: recycle the range storage of completed F1 jobs (bounded pool).
	[[nodiscard]] std::vector<Range> AcquireRanges(size_t minimum);
	// Recording thread only. `flush_buffer` (the mapped staging buffer) is flushed over
	// [flush_offset, flush_offset + flush_size) after the copies, for non-coherent memory.
	void Enqueue(std::vector<Range> ranges, Buffer* flush_buffer, uint64_t flush_offset,
	             uint64_t flush_size, std::shared_ptr<Verification> verification = {});

	// F3: one protection prefix per BDA pass; all its copy jobs follow on the same worker.
	[[nodiscard]] PageManager::ProtectBatch AcquireProtection();
	[[nodiscard]] uint64_t EnqueueProtection(PageManager::ProtectBatch batch);
	// An acquired batch the pass did not use (nothing to detach): its storage goes back to the pool.
	void ReleaseProtection(PageManager::ProtectBatch batch);

	// Recording thread only: no new job can be admitted between this guard and its write.
	void BeforeEmulatorWrite(uint64_t address, uint64_t size);
	// Foreign backing publications hold BufferCache's admission gate across their bytes.
	[[nodiscard]] uint64_t PendingSourceValue(uint64_t address, uint64_t size) const;
	void WaitSource(uint64_t value);
	// Deterministic dependency tests; destruction still drains held jobs.
	void HoldWorkerForTest(bool hold);
	void HoldWorkerAfterForTest(uint64_t value);

	[[nodiscard]] uint64_t      PendingValue() override;
	// Null unless KYTY_SUBMIT_WAIT_BEFORE_SIGNAL=1.
	[[nodiscard]] vk::Semaphore Semaphore() const override { return m_semaphore; }
	[[nodiscard]] bool          Submittable(uint64_t value) override {
		return m_completed.load(std::memory_order_acquire) >= value;
	}
	void WaitSubmittable(uint64_t value) override { WaitHost(value); }
	void WaitHost(uint64_t value) override;

private:
	struct Job {
		std::vector<Range> ranges;
		Buffer*            flush_buffer = nullptr;
		uint64_t           flush_offset = 0;
		uint64_t           flush_size   = 0;
		uint64_t           value        = 0;
		std::shared_ptr<Verification> verification;
		PageManager::ProtectBatch protection;
	};

	void Worker(std::stop_token stop);
	void Run(Job& job);

	GraphicContext&         m_graphics;
	vk::Semaphore           m_semaphore = nullptr;
	std::mutex              m_mutex;
	std::condition_variable m_available;
	std::deque<Job>         m_jobs;
	static constexpr size_t MaxRangeVectors = 64;
	static constexpr size_t MaxRetainedRanges = 4096; // at most 8 MiB of metadata
	std::vector<std::vector<Range>> m_range_pool; // protected by m_mutex
	static constexpr size_t MaxProtectBatches = 32;
	static constexpr size_t MaxRetainedProtectSpans = 4096; // at most 4 MiB of spans
	std::vector<PageManager::ProtectBatch> m_protect_pool; // protected by m_mutex
	uint64_t                m_enqueued = 0; // recording thread
	// Set while Enqueue/EnqueueProtection run. Their tickets are assigned and their sources
	// published outside m_mutex, which is only correct for one producer at a time (the recording
	// thread); a second one is fatal instead of reordering the FIFO tickets.
	std::atomic<bool>       m_producing {false};
	std::atomic<uint64_t>   m_completed {0};
	SourceCopyTracker       m_sources {m_completed};
	bool                    m_stopping = false;
	bool                    m_waiting = false; // protected by m_mutex
	bool                    m_hold = false; // HoldWorkerForTest, protected by m_mutex
	uint64_t                m_hold_after = 0; // HoldWorkerAfterForTest; zero disables, under m_mutex
	uint64_t                m_read_failures = 0; // worker thread
	std::jthread            m_worker;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_STAGINGCOPIER_H_
