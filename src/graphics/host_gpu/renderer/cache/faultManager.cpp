#include "graphics/host_gpu/renderer/cache/faultManager.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "gpu_tiler_shaders/bda_write_process_spv.h"
#include "gpu_tiler_shaders/fault_buffer_process_spv.h"
#include "graphics/shader/recompiler/CodegenOptions.h"
#include "graphics/shader/recompiler/BvhCapture.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/renderer/gpuOpProfiler.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <filesystem>
#include <fstream>
#include <string>

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

uint64_t FaultBufferSize(bool bda_writes) {
	if (ShaderRecompiler::GetCodegenOptions().astro_hardware_rt)
		return BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE + ShaderRecompiler::BvhCapture::Bytes + 512*4;
	if (ShaderRecompiler::GetCodegenOptions().bvh_capture_shader != 0)
		return BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE + ShaderRecompiler::BvhCapture::Bytes;
	return bda_writes ? BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE
	                  : PageFaultBitsetSize + sizeof(ShaderTrapRecord);
}

uint32_t CaptureValue(const char* name, uint32_t fallback, uint32_t minimum, uint32_t maximum) {
	const auto* text = std::getenv(name);
	if (!text || !*text) return fallback;
	char* end = nullptr;
	const auto value = std::strtoull(text, &end, 10);
	return end != text && *end == '\0' && value >= minimum && value <= maximum
	           ? static_cast<uint32_t>(value) : fallback;
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
                     FaultBufferSize(m_bda_writes)),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 0, AllFlags,
                        MaxPendingFaults * m_download_area_size) {
	SetVulkanObjectNameF(m_graphics.device, m_fault_buffer.Handle(), "Fault Buffer");
	EXIT_IF(m_fault_buffer.Size() > m_graphics.physical_device_properties.limits.maxStorageBufferRange);

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

bool FaultManager::BeginBvhCapture(uint64_t shader_hash, uint32_t x, uint32_t y,
                                  uint32_t z, uint32_t mode, const Buffer* indirect_args,
                                  uint64_t args_offset) {
	using namespace ShaderRecompiler::BvhCapture;
	if (!ShaderRecompiler::BvhCaptureApplies(shader_hash)) return false;
	if (m_bvh_capture_recorded) {
		// A shader can be dispatched with no active rays. Try later BVH
		// dispatches, with a bounded number of attempts, after completion.
		if (m_bvh_capture_attempts >= m_bvh_capture_shaders.size() ||
		    m_bvh_capture_outcome->load(std::memory_order_acquire) != 2) return false;
		m_bvh_capture_recorded = false;
	}
	if (ShaderRecompiler::GetCodegenOptions().bvh_capture_shader == UINT64_MAX) {
		// In all-shader mode a frequently dispatched shader with no rays
		// must not consume every attempt before a later shader can be sampled.
		const auto end = m_bvh_capture_shaders.begin() + m_bvh_capture_attempts;
		if (std::find(m_bvh_capture_shaders.begin(), end, shader_hash) != end) return false;
	}
	if (const auto* trigger = std::getenv("KYTY_BVH_CAPTURE_TRIGGER"); trigger && trigger[0]) {
		std::error_code error;
		if (!std::filesystem::exists(trigger, error) || error) return false;
	}
	static const auto skip = [] {
		const auto* value = std::getenv("KYTY_BVH_CAPTURE_SKIP");
		return value ? std::strtoull(value, nullptr, 10) : 0ull;
	}();
	if (m_bvh_capture_skips++ < skip) return false;
	(void)GetFaultBuffer();
	if (!m_bvh_capture_outcome) m_bvh_capture_outcome = std::make_shared<std::atomic<uint32_t>>(0);
	else m_bvh_capture_outcome->store(0, std::memory_order_relaxed);
	m_bvh_capture_shaders[m_bvh_capture_attempts++] = shader_hash;
	const auto capacity = CaptureValue("KYTY_BVH_CAPTURE_RECORDS", 4096, 1, MaxRecords);
	const auto sample_mask = CaptureValue("KYTY_BVH_CAPTURE_SAMPLE_MASK", 4095, 0, MaxRecords - 1);
	const bool scenes = shader_hash == AstroShader && CaptureValue("KYTY_BVH_CAPTURE_SCENE",0,0,1) == 1;
	const auto download_bytes = uint64_t(HeaderWords + capacity * RecordWords) * 4 + (scenes ? SceneBytes : 0);
	m_bvh_capture_download = std::make_shared<Buffer>(
	    m_graphics, m_scheduler, MemoryUsage::Download, 0, AllFlags, download_bytes);
	SetVulkanObjectNameF(m_graphics.device, m_bvh_capture_download->Handle(), "BVH Capture Readback");
	std::array<uint32_t, HeaderWords> header{};
	header[Enabled] = 1;
	header[FileMagic] = Magic;
	header[FileVersion] = Version;
	header[Stride] = RecordWords;
	header[Capacity] = capacity;
	header[SampleMask] = sample_mask;
	header[PcFilter] = CaptureValue("KYTY_BVH_CAPTURE_PC", 0, 0, UINT32_MAX);
	header[SceneEnabled] = scenes;
	header[ShaderLow] = static_cast<uint32_t>(shader_hash);
	header[ShaderHigh] = static_cast<uint32_t>(shader_hash >> 32);
	header[GroupsX] = x; header[GroupsY] = y; header[GroupsZ] = z; header[Mode] = mode;
	header[Indirect] = indirect_args != nullptr;
	const auto tick = m_scheduler.CurrentTick();
	header[TickLow] = static_cast<uint32_t>(tick);
	header[TickHigh] = static_cast<uint32_t>(tick >> 32);
	auto& command = m_scheduler.Current();
	command.EndRendering();
	command.RequestMemoryBarrier(vk::PipelineStageFlagBits2::eAllCommands,
	    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
	    vk::PipelineStageFlagBits2::eTransfer,
	    vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
	    BarrierOrigin::ShaderAccess);
	command.Handle().updateBuffer(m_fault_buffer.Handle(), BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE,
	                              sizeof(header), header.data());
	if (indirect_args) {
		EXIT_IF(args_offset > indirect_args->Size() ||
		        sizeof(vk::DispatchIndirectCommand) > indirect_args->Size() - args_offset);
		// Dispatch dimensions can be produced on the GPU. Capture those same bytes,
		// rather than reading possibly stale guest backing on the host.
		command.RequestMemoryBarrier(vk::PipelineStageFlagBits2::eTransfer,
		    vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eTransfer,
		    vk::AccessFlagBits2::eTransferWrite, BarrierOrigin::IndirectArgs);
		command.Handle().copyBuffer(indirect_args->Handle(), m_fault_buffer.Handle(),
		    vk::BufferCopy{args_offset, BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE + GroupsX * 4,
		                   sizeof(vk::DispatchIndirectCommand)});
	}
	m_fault_buffer.MarkContentWritten();
	command.RequestMemoryBarrier(vk::PipelineStageFlagBits2::eTransfer,
	    vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eComputeShader,
	    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
	    BarrierOrigin::ShaderAccess);
	m_bvh_capture_recorded = true;
	std::fprintf(stderr, "BVH capture armed: shader=0x%016" PRIx64 " tick=%" PRIu64 "\n", shader_hash, tick);
	return true;
}

void FaultManager::EndBvhCapture(uint64_t shader_hash) {
	using namespace ShaderRecompiler::BvhCapture;
	EXIT_IF(!m_bvh_capture_download);
	const auto capacity = CaptureValue("KYTY_BVH_CAPTURE_RECORDS",4096,1,MaxRecords);
	const uint64_t main_bytes = uint64_t(HeaderWords + capacity * RecordWords) * 4;
	const bool scenes = m_bvh_capture_download->Size() > main_bytes;
	auto& command = m_scheduler.Current();
	m_bvh_capture_download->CopyFrom(command, m_fault_buffer,
	    BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE, 0, main_bytes,
	    vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite);
	if (scenes) m_bvh_capture_download->CopyFrom(command,m_fault_buffer,
	    BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE + uint64_t(SceneOffset)*4,main_bytes,SceneBytes,
	    vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite);
	command.RequestMemoryBarrier(vk::PipelineStageFlagBits2::eTransfer,
	    vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eHost,
	    vk::AccessFlagBits2::eHostRead, BarrierOrigin::ShaderAccess);
	// No subsequent dispatch appends to this snapshot; the copy precedes the
	// clear. Its completion lease owns the staging buffer independently of us.
	m_fault_buffer.Fill(BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE + Enabled * 4, 4, 0);
	const auto tick = m_scheduler.CurrentTick();
	const auto* folder = std::getenv("KYTY_BVH_CAPTURE_DIR");
	const std::filesystem::path directory = folder && folder[0] ? folder : "_RTCapture";
	m_scheduler.DeferOperation([download = std::move(m_bvh_capture_download),
	                           outcome = m_bvh_capture_outcome, shader_hash, tick, directory,capacity,main_bytes,scenes] {
		download->Invalidate(0, download->Size());
		std::array<uint32_t, HeaderWords> header{};
		std::memcpy(header.data(), download->Mapped().data(), sizeof(header));
		const auto count = std::min(header[Count], capacity);
		outcome->store(count == 0 ? 2u : 1u, std::memory_order_release);
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		const auto path = directory / ("bvh-" + std::to_string(shader_hash) + "-" + std::to_string(tick) + ".bin");
		std::ofstream file(path, std::ios::binary);
		const auto bytes = uint64_t(HeaderWords + count * RecordWords) * 4;
		if (!error) file.write(reinterpret_cast<const char*>(download->Mapped().data()), bytes);
		if (error || !file) {
			std::fprintf(stderr, "BVH capture could not be saved: %s\n", path.string().c_str());
			return;
		}
		std::fprintf(stderr, "BVH capture saved: %s records=%u%s\n", path.string().c_str(), count,
		             count == capacity ? " (capacity reached; partial dispatch)" : "");
		if (scenes) {
			header[FileMagic] = 0x43535642; // "BVSC", coherent BLAS/instance sidecar.
			header[FileVersion] = 1;header[Stride] = SceneWords;
			header[Count] = std::min(count,MaxScenes);header[Capacity] = MaxScenes;
			const auto scene_path = directory / ("astro-scenes-" + std::to_string(shader_hash) + "-" + std::to_string(tick) + ".bin");
			std::ofstream scene_file(scene_path,std::ios::binary);
			scene_file.write(reinterpret_cast<const char*>(header.data()),sizeof(header));
			scene_file.write(reinterpret_cast<const char*>(download->Mapped().data()+main_bytes),uint64_t(header[Count])*SceneWords*4);
			std::fprintf(stderr,"Astro scene capture %s: %s slots=%u\n",scene_file?"saved":"failed",
			             scene_path.string().c_str(),header[Count]);
		}
	});
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
			if (trap.code == ShaderTrapRecord::ScalarReadFailure) {
				const auto* continue_value = std::getenv("KYTY_SCALAR_READ_PROBE_CONTINUE");
				const bool continue_capture = continue_value && std::strcmp(continue_value, "1") == 0;
				static std::atomic<uint64_t> record_sequence {0};
				const auto sequence = record_sequence.fetch_add(1, std::memory_order_relaxed);
				const auto* folder = std::getenv("KYTY_SCALAR_READ_PROBE_DIR");
				const std::filesystem::path directory = folder && *folder ? folder : "_RTCapture";
				std::error_code error;
				std::filesystem::create_directories(directory, error);
				const auto path = directory / ("scalar-read-" + std::to_string(hash) + "-" +
				                               std::to_string(trap.pc) + "-" +
				                               std::to_string(sequence) + ".bin");
				std::ofstream file(path, std::ios::binary);
				if (!error) file.write(reinterpret_cast<const char*>(&trap), sizeof(trap));
				file.close();
				std::fprintf(stderr, "Scalar read probe: %s %s\n", error || !file ? "save failed:" : "saved:",
				             path.string().c_str());
				// Preserve the completed batch's missing-page list before the diagnostic
				// exit. A zero descriptor bound can originate in an earlier absent
				// header/instance page, rather than in the descriptor size calculation.
				auto faults_path = path;
				faults_path.replace_extension(".faults.bin");
				std::ofstream faults_file(faults_path, std::ios::binary);
				if (!error) faults_file.write(reinterpret_cast<const char*>(mapped), PageFaultAreaSize);
				faults_file.close();
				std::fprintf(stderr, "Scalar read probe page list: %s %s\n",
				             error || !faults_file ? "save failed:" : "saved:",
				             faults_path.string().c_str());
				if (!continue_capture) EXIT("GPU scalar read probe (intentional diagnostic stop): hash=0x%016" PRIx64
				     " pc=0x%08x reason=%u address=0x%08x%08x size=0x%08x%08x offset=0x%08x"
				     " descriptor=[%08x,%08x,%08x,%08x] bda=0x%08x%08x\n",
				     hash, trap.pc, trap.reason, trap.address_high, trap.address_low,
				     trap.size_high, trap.size_low, trap.offset,
				     trap.descriptor[0], trap.descriptor[1], trap.descriptor[2], trap.descriptor[3],
				     trap.bda_high, trap.bda_low);
				std::fprintf(stderr, "Scalar read probe: continuing diagnostic capture after invalid read;"
				             " hash=0x%016" PRIx64 " pc=0x%x reason=%u; affected dispatch output is incomplete\n",
				             hash, trap.pc, trap.reason);
			} else {
				EXIT("GPU shader trap: hash=0x%016" PRIx64 " pc=0x%08x code=0x%02x\n",
				     hash, trap.pc, trap.code);
			}
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

}

uint64_t FaultManager::RecordBdaWrites(uint32_t slot) {
	KYTY_PROFILER_DETAIL_FUNCTION();
	KYTY_GPU_OP_SITE("fault.bda_writes");
	EXIT_IF(!m_bda_writes || slot > BdaSyncSlot || m_bda_write_ticks[slot] != 0);
	(void)GetFaultBuffer();
	if (m_bda_write_pipeline == nullptr) {
		CreateBdaWriteResources();
	}
	if (!m_bda_write_download[slot]) {
		m_bda_write_download[slot] = std::make_unique<Buffer>(
		    m_graphics, m_scheduler, MemoryUsage::Download, 0, AllFlags,
		    BdaWriteDownloadSize(m_graphics));
		SetVulkanObjectNameF(m_graphics.device, m_bda_write_download[slot]->Handle(),
		                     "BDA Written-Page Readback {}", slot);
	}
	auto&      download = *m_bda_write_download[slot];
	const auto size     = download.Size();
	auto*      mapped   = download.Mapped().data();
	// The compactor overwrites every reported entry. Clear just its counter
	// and the dropped count instead of touching a 512 KiB list per dispatch.
	std::memset(mapped, 0, sizeof(uint64_t));
	std::memset(mapped + WrittenPageAreaSize, 0, sizeof(uint32_t));
	// VMA rounds each range to non-coherent atoms; untouched list bytes need no flush.
	download.Flush(0, sizeof(uint64_t));
	download.Flush(WrittenPageAreaSize, sizeof(uint32_t));

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
	auto               command = m_scheduler.Current().Sink();
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
	command.copyBuffer(m_fault_buffer.Handle(), download.Handle(), 1, &dropped_copy);
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

	m_bda_write_ticks[slot] = m_scheduler.CurrentTick();
	return m_bda_write_ticks[slot];
}

void FaultManager::ParseBdaWrites(uint32_t slot, BdaWrites& result) {
	EXIT_IF(slot > BdaSyncSlot || m_bda_write_ticks[slot] == 0);
	// Its caller has already waited for the native timeline. No GPU-owned
	// cache metadata is inspected or changed on the completion runner.
	auto& download = *m_bda_write_download[slot];
	const auto size = download.Size();
	const auto* mapped = download.Mapped().data();
	result.tick = m_bda_write_ticks[slot];
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

struct FaultManager::DroppedChecks {
	static constexpr uint32_t Slots = 16;
	std::unique_ptr<Buffer>                download;
	std::array<std::atomic<bool>, Slots>   pending {};
};

void FaultManager::QueueBdaDroppedCheck(uint64_t shader_hash) {
	KYTY_GPU_OP_SITE("fault.bda_dropped_check");
	EXIT_IF(!m_bda_writes);
	(void)GetFaultBuffer();
	if (!m_dropped_checks) {
		m_dropped_checks           = std::make_shared<DroppedChecks>();
		m_dropped_checks->download = std::make_unique<Buffer>(
		    m_graphics, m_scheduler, MemoryUsage::Download, 0, AllFlags,
		    std::max<uint64_t>(DroppedChecks::Slots * sizeof(uint32_t),
		                       m_graphics.physical_device_properties.limits.nonCoherentAtomSize));
	}
	auto&      checks = *m_dropped_checks;
	const auto slot   = m_dropped_check_next % DroppedChecks::Slots;
	if (checks.pending[slot].load(std::memory_order_acquire)) {
		return; // the count stays in the fault buffer for a later check (or a settle)
	}
	checks.pending[slot].store(true, std::memory_order_relaxed);
	m_dropped_check_next++;

	// Producers: the dispatch's atomic adds. Consumers: this copy, then the clear (write-after-read),
	// then later dispatches' adds and the host read of the slot.
	vk::BufferMemoryBarrier2 count_barrier {};
	count_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	count_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	count_barrier.dstStageMask  = vk::PipelineStageFlagBits2::eTransfer;
	count_barrier.dstAccessMask =
	    vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite;
	count_barrier.buffer = m_fault_buffer.Handle();
	count_barrier.offset = BufferCache::BDA_DROPPED_WRITES_OFFSET;
	count_barrier.size   = sizeof(uint32_t);
	vk::DependencyInfo dependency {};
	dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &count_barrier;

	m_scheduler.EndRendering();
	// Encoded with KYTY_CP_RECORDER: no recorder drain after the dispatch.
	const auto sink = m_scheduler.Current().Sink();
	sink.pipelineBarrier2(dependency);
	const vk::BufferCopy copy {BufferCache::BDA_DROPPED_WRITES_OFFSET, slot * sizeof(uint32_t),
	                           sizeof(uint32_t)};
	sink.copyBuffer(m_fault_buffer.Handle(), checks.download->Handle(), 1, &copy);
	auto clear_barrier          = count_barrier;
	clear_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
	clear_barrier.srcAccessMask = vk::AccessFlagBits2::eNone;
	clear_barrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
	dependency.pBufferMemoryBarriers = &clear_barrier;
	sink.pipelineBarrier2(dependency);
	sink.fillBuffer(m_fault_buffer.Handle(), BufferCache::BDA_DROPPED_WRITES_OFFSET,
	                sizeof(uint32_t), 0);
	m_fault_buffer.MarkContentWritten();
	auto after_count          = count_barrier;
	after_count.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
	after_count.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	after_count.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	after_count.dstAccessMask =
	    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
	vk::BufferMemoryBarrier2 to_host {};
	to_host.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
	to_host.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	to_host.dstStageMask  = vk::PipelineStageFlagBits2::eHost;
	to_host.dstAccessMask = vk::AccessFlagBits2::eHostRead;
	to_host.buffer        = checks.download->Handle();
	to_host.offset        = copy.dstOffset;
	to_host.size          = sizeof(uint32_t);
	const std::array after {after_count, to_host};
	dependency.bufferMemoryBarrierCount = static_cast<uint32_t>(after.size());
	dependency.pBufferMemoryBarriers    = after.data();
	sink.pipelineBarrier2(dependency);

	m_scheduler.DeferOperation([state = m_dropped_checks, slot, shader_hash] {
		auto& download = *state->download;
		download.Invalidate(0, download.Size());
		uint32_t dropped = 0;
		std::memcpy(&dropped, download.Mapped().data() + slot * sizeof(uint32_t), sizeof(dropped));
		state->pending[slot].store(false, std::memory_order_release);
		if (dropped == 0) {
			return;
		}
		Profiler::CountFrameEvent(Profiler::FrameEvent::BdaCandidateDroppedWrites, dropped);
		static std::atomic<uint32_t> logged {0};
		if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("BDA candidates: shader=0x%016" PRIx64 " dropped %u writes to pages without a cache "
			     "buffer\n",
			     shader_hash, dropped);
		}
		// Parsed as SettleBdaWrites parses it: a whole decimal number.
		static const bool verify = [] {
			const auto* value = std::getenv("KYTY_BDA_WRITES_VERIFY");
			char*       end   = nullptr;
			return value != nullptr && std::strtoull(value, &end, 10) != 0 && end != value &&
			       *end == '\0';
		}();
		if (verify) {
			EXIT("KYTY_BDA_WRITES_VERIFY: candidate shader 0x%016" PRIx64 " dropped %u writes\n",
			     shader_hash, dropped);
		}
	});
}

void FaultManager::ReleaseBdaWrites(uint32_t slot) {
	EXIT_IF(slot > BdaSyncSlot || m_bda_write_ticks[slot] == 0);
	m_bda_write_ticks[slot] = 0;
}

void FaultManager::CollectBdaWrites(BdaWrites& result) {
	const auto tick = RecordBdaWrites(BdaSyncSlot);
	{
		Profiler::ScopedGpuWaitReason wait_reason(Profiler::FrameWait::BdaSettle);
		m_scheduler.Wait(tick);
	}
	ParseBdaWrites(BdaSyncSlot, result);
	ReleaseBdaWrites(BdaSyncSlot);
}

} // namespace Libs::Graphics
