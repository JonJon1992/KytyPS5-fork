#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_

#include "common/abi.h"
#include "graphics/shader/shaderBindings.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <cstdint>
#include <memory>

namespace Libs::Graphics {

class BufferCache;

class FaultManager {
	static constexpr size_t MaxPendingFaults = 8;

public:
	FaultManager(GraphicContext& graphics, CommandScheduler& scheduler, BufferCache& buffer_cache);
	~FaultManager();
	KYTY_CLASS_NO_COPY(FaultManager);

	[[nodiscard]] Buffer* GetFaultBuffer() noexcept;
	void                  ProcessFaultBuffer();

	// KYTY_BDA_WRITES_SHADERS: what the dispatches recorded since the previous collection wrote
	// through BDA, from the fault buffer's written-page bitmap and dropped-write count.
	struct BdaWrites {
		RangeSet written;           // caching pages, merged into runs
		uint64_t pages    = 0;      // pages the compaction counted (written may hold fewer)
		uint64_t tick     = 0;      // the scheduler tick that recorded the writers
		uint32_t dropped  = 0;      // writes to pages without a cache buffer
		bool     overflow = false;  // more pages than the compaction holds: written is partial
	};
	// Records the bitmap's compaction (which clears it) and the dropped-count readback, then
	// submits and waits for them (phase 0 of the BDA-writes design: a synchronous settle). GPU
	// thread; only with KYTY_BDA_WRITES_SHADERS. `result` keeps its storage between calls.
	void CollectBdaWrites(BdaWrites& result);
	// KYTY_BDA_WRITES=candidates, which has no settle: after the dispatch, copies the count of
	// writes dropped on pages without a cache buffer to a readback slot and clears it. The slot is
	// read once the recording completes, with no wait; a nonzero count is reported
	// (BdaCandidateDroppedWrites, a log line) and fatal with KYTY_BDA_WRITES_VERIFY. While every
	// slot still waits for its recording, the count carries over to the next check.
	void QueueBdaDroppedCheck(uint64_t shader_hash);
	static constexpr uint32_t BdaWriteSlots = 8;
	// CollectBdaWrites' own slot, past the deferred producers' ring: a synchronous settle (the
	// fallback of KYTY_BDA_WRITES=deferred) never waits for or reuses a pending producer's slot.
	static constexpr uint32_t BdaSyncSlot = BdaWriteSlots;
	// Record belongs to the GPU owner; parse belongs to the native-completion
	// runner. A slot is retained until the owner has applied its result.
	[[nodiscard]] uint64_t RecordBdaWrites(uint32_t slot);
	void ParseBdaWrites(uint32_t slot, BdaWrites& result);
	void ReleaseBdaWrites(uint32_t slot);

private:
	void CreateBdaWriteResources();

	GraphicContext&                            m_graphics;
	CommandScheduler&                          m_scheduler;
	BufferCache&                               m_buffer_cache;
	size_t                                     m_download_area_size;
	bool                                       m_initialized = false;
	bool                                       m_bda_writes  = false;
	Buffer                                     m_fault_buffer;
	Buffer                                     m_download_buffer;
	std::array<uint64_t, MaxPendingFaults>      m_fault_areas {};
	uint32_t                                   m_current_area = 0;
	vk::DescriptorSetLayout                    m_fault_process_desc_layout = nullptr;
	vk::Pipeline                               m_fault_process_pipeline = nullptr;
	vk::PipelineLayout                         m_fault_process_pipeline_layout = nullptr;
	// KYTY_BDA_WRITES_SHADERS, created at the first collection: the 64Ki-page compaction of the
	// written-page bitmap and its host-visible result (page list, then the dropped count).
	vk::Pipeline                               m_bda_write_pipeline = nullptr;
	// QueueBdaDroppedCheck's readback slots, shared with the completion callbacks.
	struct DroppedChecks;
	std::shared_ptr<DroppedChecks>             m_dropped_checks;
	uint32_t                                   m_dropped_check_next = 0;
	std::array<std::unique_ptr<Buffer>, BdaWriteSlots + 1> m_bda_write_download;
	std::array<uint64_t, BdaWriteSlots + 1>              m_bda_write_ticks {};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_
