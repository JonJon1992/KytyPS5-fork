#include "graphics/host_gpu/renderer/cache/faultManager.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "gpu_tiler_shaders/bda_write_process_spv.h"
#include "gpu_tiler_shaders/fault_buffer_process_spv.h"
#include "graphics/shader/recompiler/CodegenOptions.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/renderer/gpuOpProfiler.h"

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstring>
#include <limits>

namespace Libs::Graphics {

namespace {

constexpr size_t MaxPageFaults       = 1024;
constexpr size_t PageFaultAreaSize   = MaxPageFaults * sizeof(uint64_t);
constexpr size_t PageFaultBitsetSize = BufferCache::CACHING_NUMPAGES / 8;

// KYTY_BDA_WRITES_SHADERS: bda_write_process.comp's capacity (MAX_PAGE_FAULTS, CMakeLists.txt).
constexpr size_t MaxWrittenPages       = 65536;
constexpr size_t WrittenPageAreaSize   = MaxWrittenPages * sizeof(uint64_t);

static_assert(PageFaultBitsetSize == BufferCache::FAULT_BITMAP_BYTES);
static_assert(BufferCache::BDA_WRITE_BITMAP_OFFSET >= PageFaultBitsetSize + sizeof(ShaderTrapRecord),
              "the written-page bitmap must not overlap the trap record");

size_t DownloadAreaSize(const GraphicContext& graphics) {
	const auto& limits = graphics.physical_device_properties.limits;
	const auto  alignment =
	    std::max(limits.nonCoherentAtomSize, limits.minStorageBufferOffsetAlignment);
	return (PageFaultAreaSize + sizeof(ShaderTrapRecord) + alignment - 1) & ~(alignment - 1);
}

// One page-bitmap compaction (fault_buffer_process.comp, built with some MAX_PAGE_FAULTS) on the
// fault manager's push-descriptor layout: binding 0 the bitmap range, binding 1 the page list.
vk::Pipeline CreateCompactionPipeline(vk::Device device, vk::PipelineLayout layout,
                                      std::span<const uint32_t> code, const char* name) {
	const auto                        module = CompileSPV(code, device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = layout;
	vk::Pipeline pipeline = nullptr;
	const auto   result   = device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr,
	                                                      &pipeline);
	device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create fault-buffer pipeline");
	SetVulkanObjectNameF(device, pipeline, "{}", name);
	return pipeline;
}

// The written-page list, then the dropped-write count, rounded to whole non-coherent atoms.
size_t BdaWriteDownloadSize(const GraphicContext& graphics) {
	const auto& limits = graphics.physical_device_properties.limits;
	const auto  alignment =
	    std::max(limits.nonCoherentAtomSize, limits.minStorageBufferOffsetAlignment);
	return (WrittenPageAreaSize + sizeof(uint32_t) + alignment - 1) & ~(alignment - 1);
}

} // namespace

FaultManager::FaultManager(GraphicContext& graphics, CommandScheduler& scheduler,
                           BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_buffer_cache(buffer_cache),
      m_download_area_size(DownloadAreaSize(graphics)),
      m_bda_writes(ShaderRecompiler::BdaWritesEnabled()),
      // The written-page bitmap and the dropped count exist only while a shader is listed.
      m_fault_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                     m_bda_writes ? BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE
                                  : PageFaultBitsetSize + sizeof(ShaderTrapRecord)),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 0, AllFlags,
                        MaxPendingFaults * m_download_area_size) {
	SetVulkanObjectNameF(m_graphics.device, m_fault_buffer.Handle(), "Fault Buffer");

	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = std::size(bindings);
	layout_info.pBindings    = bindings;
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr,
	                                                &m_fault_process_desc_layout),
	    "create fault-buffer descriptor layout");

	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount = 1;
	pipeline_layout_info.pSetLayouts    = &m_fault_process_desc_layout;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                           &m_fault_process_pipeline_layout),
	    "create fault-buffer pipeline layout");

	m_fault_process_pipeline =
	    CreateCompactionPipeline(m_graphics.device, m_fault_process_pipeline_layout,
	                             FAULT_BUFFER_PROCESS_SPV, "Fault Buffer Parser");
}

FaultManager::~FaultManager() {
	if (m_bda_write_pipeline != nullptr) {
		m_graphics.device.destroyPipeline(m_bda_write_pipeline, nullptr);
	}
	m_graphics.device.destroyPipeline(m_fault_process_pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(m_fault_process_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_fault_process_desc_layout, nullptr);
}

Buffer* FaultManager::GetFaultBuffer() noexcept {
	// Construction happens before the scheduler is active. Initialize on the GPU
	// thread at first binding, before any shader can read or claim the record.
	if (!m_initialized) {
		m_fault_buffer.Fill(0, m_fault_buffer.Size(), 0);
		m_initialized = true;
	}
	return &m_fault_buffer;
}

void FaultManager::ProcessFaultBuffer() {
	HangTrace::SyncResource sync_resource(
	    reinterpret_cast<uint64_t>(static_cast<VkBuffer>(m_download_buffer.Handle())),
	    m_download_area_size);
	KYTY_PROFILER_DETAIL_FUNCTION();
	(void)GetFaultBuffer();
	KYTY_GPU_OP_SITE("fault.process");
	if (const auto wait_tick = m_fault_areas[m_current_area]; wait_tick != 0) {
		m_scheduler.Wait(wait_tick);
		m_scheduler.PopPendingOperations();
	}

	const auto offset = m_current_area * m_download_area_size;
	auto*      mapped = m_download_buffer.Mapped().data() + offset;
	std::memset(mapped, 0, m_download_area_size);
	m_download_buffer.Flush(offset, m_download_area_size);

	vk::BufferMemoryBarrier2 pre_barrier {};
	pre_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	pre_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	pre_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer;
	pre_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferRead;
	pre_barrier.buffer        = m_fault_buffer.Handle();
	pre_barrier.offset        = 0;
	pre_barrier.size           = m_fault_buffer.Size();
	// Covers the parser's writes and the trap record clear (fill) below.
	auto post_barrier         = pre_barrier;
	post_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer;
	post_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite;
	post_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	post_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
	                             vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite;

	const vk::DescriptorBufferInfo infos[] {
	    {m_fault_buffer.Handle(), 0, PageFaultBitsetSize},
	    {m_download_buffer.Handle(), offset, PageFaultAreaSize},
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}

	m_scheduler.EndRendering();
	auto command = m_scheduler.Current().Handle();
	vk::DependencyInfo dependency {};
	dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &pre_barrier;
	command.pipelineBarrier2(dependency);
	m_scheduler.Current().BindPipeline(vk::PipelineBindPoint::eCompute, m_fault_process_pipeline);
	m_scheduler.Current().PushDescriptors(vk::PipelineBindPoint::eCompute,
	                             m_fault_process_pipeline_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());
	const auto num_threads    = BufferCache::CACHING_NUMPAGES / 32;
	const auto num_workgroups = (num_threads + 63) / 64;
	command.dispatch(static_cast<uint32_t>(num_workgroups), 1, 1);
	// Preserve the trap record in readback, then release it for the next attempt. The clear
	// only has to wait for the copy's read (write-after-read): an execution dependency.
	const vk::BufferCopy trap_copy {PageFaultBitsetSize, offset + PageFaultAreaSize,
	                                sizeof(ShaderTrapRecord)};
	command.copyBuffer(m_fault_buffer.Handle(), m_download_buffer.Handle(), trap_copy);
	vk::BufferMemoryBarrier2 trap_barrier {};
	trap_barrier.srcStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	trap_barrier.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	trap_barrier.dstAccessMask       = vk::AccessFlagBits2::eTransferWrite;
	trap_barrier.buffer              = m_fault_buffer.Handle();
	trap_barrier.offset              = PageFaultBitsetSize;
	trap_barrier.size                = sizeof(ShaderTrapRecord);
	dependency.pBufferMemoryBarriers = &trap_barrier;
	command.pipelineBarrier2(dependency);
	command.fillBuffer(m_fault_buffer.Handle(), PageFaultBitsetSize, sizeof(ShaderTrapRecord), 0);
	m_fault_buffer.MarkContentWritten();
	vk::BufferMemoryBarrier2 download_barrier {};
	download_barrier.srcStageMask =
	    vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer;
	download_barrier.srcAccessMask =
	    vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite;
	download_barrier.dstStageMask    = vk::PipelineStageFlagBits2::eHost;
	download_barrier.dstAccessMask   = vk::AccessFlagBits2::eHostRead;
	download_barrier.buffer          = m_download_buffer.Handle();
	download_barrier.offset          = offset;
	download_barrier.size            = m_download_area_size;
	// One dependency publishes the fault buffer to later commands and the readback to the host.
	const std::array post_barriers {post_barrier, download_barrier};
	dependency.bufferMemoryBarrierCount = static_cast<uint32_t>(post_barriers.size());
	dependency.pBufferMemoryBarriers    = post_barriers.data();
	command.pipelineBarrier2(dependency);

	const auto area = m_current_area;
	m_scheduler.DeferOperation([this, mapped, offset, area] {
		m_download_buffer.Invalidate(offset, m_download_area_size);
		ShaderTrapRecord trap;
		std::memcpy(&trap, mapped + PageFaultAreaSize, sizeof(trap));
		if (trap.claimed != 0) {
			const auto hash = (uint64_t{trap.shader_hash_high} << 32) | trap.shader_hash_low;
			EXIT("GPU shader trap: hash=0x%016" PRIx64 " pc=0x%08x code=0x%02x\n",
			     hash, trap.pc, trap.code);
		}

		RangeSet    fault_ranges;
		const auto* faults = std::bit_cast<const uint64_t*>(mapped);
		const auto count = std::min<uint64_t>(faults[0], MaxPageFaults - 1);
		if (faults[0] > count) {
			LOGF("GPU page-fault report truncated: %" PRIu64 " entries, %" PRIu64 " recorded\n", faults[0], count);
		}
		for (uint32_t index = 1; index <= count; ++index) {
			fault_ranges.Add(faults[index], BufferCache::CACHING_PAGESIZE);
			LOGF("Accessed non-GPU cached memory at 0x%016" PRIx64 "\n", faults[index]);
		}
		fault_ranges.ForEach([this](uint64_t start, uint64_t end) {
			EXIT_IF(end - start > std::numeric_limits<uint32_t>::max());
			(void)m_buffer_cache.FindBuffer(start, end - start);
		});
		m_fault_areas[area] = 0;
	});

	m_fault_areas[m_current_area++] = m_scheduler.CurrentTick();
	m_current_area %= MaxPendingFaults;
}

void FaultManager::CreateBdaWriteResources() {
	m_bda_write_pipeline =
	    CreateCompactionPipeline(m_graphics.device, m_fault_process_pipeline_layout,
	                             BDA_WRITE_PROCESS_SPV, "BDA Written-Page Parser");
	m_bda_write_download =
	    std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0, AllFlags,
	                             BdaWriteDownloadSize(m_graphics));
	SetVulkanObjectNameF(m_graphics.device, m_bda_write_download->Handle(),
	                     "BDA Written-Page Readback");
}

void FaultManager::CollectBdaWrites(BdaWrites& result) {
	KYTY_PROFILER_DETAIL_FUNCTION();
	KYTY_GPU_OP_SITE("fault.bda_writes");
	EXIT_IF(!m_bda_writes);
	(void)GetFaultBuffer();
	if (m_bda_write_pipeline == nullptr) {
		CreateBdaWriteResources();
	}
	// One readback area: the wait below completes the previous collection before this one.
	auto&      download = *m_bda_write_download;
	const auto size     = download.Size();
	auto*      mapped   = download.Mapped().data();
	std::memset(mapped, 0, WrittenPageAreaSize + sizeof(uint32_t));
	download.Flush(0, size);

	// Producers: the dispatches that set bitmap bits and the dropped count (any stage, through
	// BDA). Consumers: the compaction (reads and clears the bitmap) and the count's copy and clear.
	vk::BufferMemoryBarrier2 pre_barrier {};
	pre_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	pre_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	pre_barrier.dstStageMask =
	    vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer;
	pre_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead |
	                            vk::AccessFlagBits2::eShaderWrite |
	                            vk::AccessFlagBits2::eTransferRead;
	pre_barrier.buffer = m_fault_buffer.Handle();
	pre_barrier.offset = BufferCache::BDA_WRITE_BITMAP_OFFSET;
	pre_barrier.size   = BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE -
	                   BufferCache::BDA_WRITE_BITMAP_OFFSET;

	const vk::DescriptorBufferInfo infos[] {
	    {m_fault_buffer.Handle(), BufferCache::BDA_WRITE_BITMAP_OFFSET,
	     BufferCache::FAULT_BITMAP_BYTES},
	    {download.Handle(), 0, WrittenPageAreaSize},
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}

	m_scheduler.EndRendering();
	auto               command = m_scheduler.Current().Handle();
	vk::DependencyInfo dependency {};
	dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &pre_barrier;
	command.pipelineBarrier2(dependency);
	m_scheduler.Current().BindPipeline(vk::PipelineBindPoint::eCompute, m_bda_write_pipeline);
	m_scheduler.Current().PushDescriptors(vk::PipelineBindPoint::eCompute,
	                                      m_fault_process_pipeline_layout, 0,
	                                      static_cast<uint32_t>(writes.size()), writes.data());
	const auto num_threads    = BufferCache::CACHING_NUMPAGES / 32;
	const auto num_workgroups = (num_threads + 63) / 64;
	command.dispatch(static_cast<uint32_t>(num_workgroups), 1, 1);
	// Read the dropped count, then clear it once the copy has read it (write-after-read).
	const vk::BufferCopy dropped_copy {BufferCache::BDA_DROPPED_WRITES_OFFSET, WrittenPageAreaSize,
	                                   sizeof(uint32_t)};
	command.copyBuffer(m_fault_buffer.Handle(), download.Handle(), dropped_copy);
	vk::BufferMemoryBarrier2 dropped_barrier {};
	dropped_barrier.srcStageMask     = vk::PipelineStageFlagBits2::eTransfer;
	dropped_barrier.dstStageMask     = vk::PipelineStageFlagBits2::eTransfer;
	dropped_barrier.dstAccessMask    = vk::AccessFlagBits2::eTransferWrite;
	dropped_barrier.buffer           = m_fault_buffer.Handle();
	dropped_barrier.offset           = BufferCache::BDA_DROPPED_WRITES_OFFSET;
	dropped_barrier.size             = sizeof(uint32_t);
	dependency.pBufferMemoryBarriers = &dropped_barrier;
	command.pipelineBarrier2(dependency);
	command.fillBuffer(m_fault_buffer.Handle(), BufferCache::BDA_DROPPED_WRITES_OFFSET,
	                   sizeof(uint32_t), 0);
	m_fault_buffer.MarkContentWritten();
	// The cleared bitmap and count to later writers; the page list and count to the host.
	auto post_barrier          = pre_barrier;
	post_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader |
	                            vk::PipelineStageFlagBits2::eTransfer;
	post_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite |
	                             vk::AccessFlagBits2::eTransferWrite;
	post_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	post_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead |
	                             vk::AccessFlagBits2::eShaderWrite;
	vk::BufferMemoryBarrier2 download_barrier {};
	download_barrier.srcStageMask =
	    vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer;
	download_barrier.srcAccessMask =
	    vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite;
	download_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eHost;
	download_barrier.dstAccessMask = vk::AccessFlagBits2::eHostRead;
	download_barrier.buffer        = download.Handle();
	download_barrier.offset        = 0;
	download_barrier.size          = size;
	const std::array post_barriers {post_barrier, download_barrier};
	dependency.bufferMemoryBarrierCount = static_cast<uint32_t>(post_barriers.size());
	dependency.pBufferMemoryBarriers    = post_barriers.data();
	command.pipelineBarrier2(dependency);

	result.tick = m_scheduler.CurrentTick();
	{
		Profiler::ScopedGpuWaitReason wait_reason(Profiler::FrameWait::BdaSettle);
		m_scheduler.Wait(result.tick);
	}
	download.Invalidate(0, size);

	result.written.Clear();
	const auto* pages  = std::bit_cast<const uint64_t*>(mapped);
	const auto  count  = static_cast<uint64_t>(std::bit_cast<const uint32_t*>(mapped)[0]);
	const auto  stored = std::min<uint64_t>(count, MaxWrittenPages - 1);
	for (uint64_t index = 1; index <= stored; ++index) {
		result.written.Add(pages[index], BufferCache::CACHING_PAGESIZE);
	}
	result.pages    = count;
	result.overflow = count > stored;
	std::memcpy(&result.dropped, mapped + WrittenPageAreaSize, sizeof(result.dropped));
}

} // namespace Libs::Graphics
