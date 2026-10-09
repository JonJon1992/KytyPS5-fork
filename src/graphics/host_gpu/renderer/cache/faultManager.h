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
	static constexpr uint32_t BdaWriteSlots = 8;
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
	std::array<std::unique_ptr<Buffer>, BdaWriteSlots> m_bda_write_download;
	std::array<uint64_t, BdaWriteSlots>              m_bda_write_ticks {};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_
