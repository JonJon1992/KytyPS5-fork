#include "common/hangTrace.h"
#include "graphics/host_gpu/renderer/pipeline/bindlessTable.h"

#include "common/assert.h"
#include "common/liveSwitch.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/samplerCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shaderBindings.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <array>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics {

BindlessTable::BindlessTable(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler) {
    graphics.bindless_enabled = false;
    graphics.bindless_layout = nullptr;
    graphics.bindless_set = nullptr;
	if (!graphics.bindless_supported) {
		return;
	}
	m_images_per_array =
	    std::min(MaxImagesPerArray, graphics.bindless_images_per_array);
	if (m_images_per_array < PlaceholderColors) {
		LOGF("Bindless table: the device allows no update-after-bind sampled images\n");
		return;
	}

	constexpr auto image_flags = vk::DescriptorBindingFlagBits::ePartiallyBound |
	                             vk::DescriptorBindingFlagBits::eUpdateAfterBind |
	                             vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending;
	// The buffers are written once, here, so they need no update-after-bind.
	constexpr auto buffer_flags = vk::DescriptorBindingFlags {};
	// Images and samplers share the sampled-image update-after-bind capability gate.
	m_samplers_per_array = std::min(MaxSamplers, graphics.bindless_samplers_per_array);
	const std::array<vk::DescriptorSetLayoutBinding, 7> bindings {{
	    {Images2D, vk::DescriptorType::eSampledImage, m_images_per_array, vk::ShaderStageFlagBits::eAll,
	     nullptr},
	    {Images2DArray, vk::DescriptorType::eSampledImage, m_images_per_array,
	     vk::ShaderStageFlagBits::eAll, nullptr},
	    {ImagesCube, vk::DescriptorType::eSampledImage, m_images_per_array,
	     vk::ShaderStageFlagBits::eAll, nullptr},
	    {Images3D, vk::DescriptorType::eSampledImage, m_images_per_array, vk::ShaderStageFlagBits::eAll,
	     nullptr},
	    {Translation, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr},
	    {Feedback, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr},
	    {Samplers, vk::DescriptorType::eSampler, std::max(m_samplers_per_array, 1u),
	     vk::ShaderStageFlagBits::eAll, nullptr},
	}};
	const std::array<vk::DescriptorBindingFlags, 7> binding_flags {
	    image_flags, image_flags, image_flags, image_flags, buffer_flags, buffer_flags, image_flags};
	vk::DescriptorSetLayoutBindingFlagsCreateInfo flags_info {};
	flags_info.bindingCount  = static_cast<uint32_t>(binding_flags.size());
	flags_info.pBindingFlags = binding_flags.data();
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.pNext        = &flags_info;
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
	layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
	layout_info.pBindings    = bindings.data();
    vk::DescriptorSetLayoutSupport support {};
    graphics.device.getDescriptorSetLayoutSupport(&layout_info, &support);
    if (!support.supported ||
        graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_layout) != vk::Result::eSuccess) {
        return;
    }

	const std::array<vk::DescriptorPoolSize, 3> sizes {{
	    {vk::DescriptorType::eSampledImage, m_images_per_array * ImageArrays},
	    {vk::DescriptorType::eStorageBuffer, 2},
	    {vk::DescriptorType::eSampler, std::max(m_samplers_per_array, 1u)},
	}};
	vk::DescriptorPoolCreateInfo pool_info {};
	pool_info.flags         = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
	pool_info.maxSets       = 1;
	pool_info.poolSizeCount = static_cast<uint32_t>(sizes.size());
	pool_info.pPoolSizes    = sizes.data();
	RequireVulkanSuccess(graphics.device.createDescriptorPool(&pool_info, nullptr, &m_pool),
	                     "create bindless descriptor pool");

	vk::DescriptorSetAllocateInfo allocate_info {};
	allocate_info.descriptorPool     = m_pool;
	allocate_info.descriptorSetCount = 1;
	allocate_info.pSetLayouts        = &m_layout;
	RequireVulkanSuccess(graphics.device.allocateDescriptorSets(&allocate_info, &m_set),
	                     "allocate bindless descriptor set");

	// A region is written only while no command that has not completed may read it (Publish
	// moves a heap whose region is in use). Every heap is eagerly resolved, so feedback is
	// diagnostic and never races a host read/clear.
	m_translation = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Stream, 0,
	                                         AllFlags, TranslationEntries * sizeof(uint32_t));
	m_feedback    = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Stream, 0,
	                                         AllFlags, TranslationEntries * sizeof(uint32_t));
	EXIT_IF(m_translation->Mapped().empty() || m_feedback->Mapped().empty());
	SetVulkanObjectNameF(graphics.device, m_translation->Handle(), "Bindless Translation");
	SetVulkanObjectNameF(graphics.device, m_feedback->Handle(), "Bindless Feedback");

	const std::array<vk::DescriptorBufferInfo, 2> buffer_infos {{
	    {m_translation->Handle(), 0, VK_WHOLE_SIZE},
	    {m_feedback->Handle(), 0, VK_WHOLE_SIZE},
	}};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t i = 0; i < writes.size(); i++) {
		writes[i].dstSet          = m_set;
		writes[i].dstBinding      = Translation + i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[i].pBufferInfo     = &buffer_infos[i];
	}
	graphics.device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0,
	                                     nullptr);
	CreatePlaceholders(scheduler);
	graphics.bindless_layout = m_layout;
	graphics.bindless_set    = m_set;
	graphics.bindless_enabled = true;
	LOGF("Bindless table: %u images per array, %u translation entries, %u samplers\n",
	     m_images_per_array, TranslationEntries, m_samplers_per_array);
}

BindlessTable::SamplerHeap* BindlessTable::FindOrCreateSamplerHeap(uint64_t base,
                                                                   uint32_t table_offset,
                                                                   uint32_t record_stride,
                                                                   uint32_t flags) {
	auto& indexed = m_sampler_heap_index[HeapPlace {base, table_offset, record_stride, flags}];
	if (indexed != nullptr) {
		return indexed;
	}
	auto& heap         = m_sampler_heaps.emplace_back();
	heap.base          = base;
	heap.table_offset  = table_offset;
	heap.record_stride = record_stride;
	heap.flags         = flags;
	indexed            = &heap;
	return &heap;
}

void BindlessTable::WriteDefaultSampler(vk::Sampler sampler) {
	if (m_default_sampler_written || m_set == nullptr) {
		return;
	}
	vk::DescriptorImageInfo info {sampler, nullptr, vk::ImageLayout::eUndefined};
	vk::WriteDescriptorSet  write {};
	write.dstSet          = m_set;
	write.dstBinding      = Samplers;
	write.dstArrayElement = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eSampler;
	write.pImageInfo      = &info;
	m_graphics.device.updateDescriptorSets(1, &write, 0, nullptr);
	m_default_sampler_written = true;
}

bool BindlessTable::MirrorSamplerHeap(SamplerHeap&                              heap,
                                      std::span<const std::array<uint32_t, 4>> records,
                                      SamplerCache&                             cache) {
	const auto same_prefix =
	    records.size() >= heap.records.size() &&
	    std::equal(heap.records.begin(), heap.records.end(), records.begin());
	if (same_prefix && records.size() == heap.records.size()) {
		return true;
	}
	uint32_t first = static_cast<uint32_t>(heap.records.size());
	if (!same_prefix || records.size() > heap.capacity || heap.region == 0) {
		const auto capacity = static_cast<uint32_t>(records.size());
		if (m_samplers_per_array <= 1u || capacity > m_samplers_per_array - m_next_sampler_slot) {
			static std::atomic<uint32_t> reported = 0;
			if (reported.fetch_add(1) < 8) {
				LOGF("Bindless samplers: array full (%u of %u slots), heap 0x%016" PRIx64
				     " keeps %zu records\n",
				     m_next_sampler_slot, m_samplers_per_array, heap.base, heap.records.size());
			}
			return false;
		}
		heap.region   = m_next_sampler_slot;
		heap.capacity = capacity;
		m_next_sampler_slot += capacity;
		first = 0;
	}
	std::vector<vk::DescriptorImageInfo> infos;
	infos.reserve(records.size() - first);
	for (size_t key = first; key < records.size(); key++) {
		ShaderSamplerResource descriptor;
		std::copy(records[key].begin(), records[key].end(), descriptor.fields);
		if ((heap.flags & SamplerDepthCompare) == 0u) {
			descriptor.fields[0] &= ~(0x7u << 12u);
		}
		if ((heap.flags & SamplerPointFiltering) != 0u) {
			descriptor.SetPointFiltering();
		}
		infos.push_back({cache.GetSampler(descriptor, (heap.flags & SamplerIntegerBorder) != 0u),
		                 nullptr, vk::ImageLayout::eUndefined});
	}
	if (!infos.empty()) {
		vk::WriteDescriptorSet write {};
		write.dstSet          = m_set;
		write.dstBinding      = Samplers;
		write.dstArrayElement = heap.region + first;
		write.descriptorCount = static_cast<uint32_t>(infos.size());
		write.descriptorType  = vk::DescriptorType::eSampler;
		write.pImageInfo      = infos.data();
		m_graphics.device.updateDescriptorSets(1, &write, 0, nullptr);
	}
	static std::atomic<uint32_t> logged = 0;
	if (logged.fetch_add(1) < 32) {
		LOGF("Bindless samplers: heap 0x%016" PRIx64 "+0x%x flags=%u region=%u records=%zu"
		     " (%u new)\n",
		     heap.base, heap.table_offset, heap.flags, heap.region, records.size(),
		     static_cast<uint32_t>(infos.size()));
	}
	heap.records.assign(records.begin(), records.end());
	return true;
}

void BindlessTable::CreatePlaceholders(CommandScheduler& /*scheduler*/) {
	struct Kind {
		vk::ImageType        type;
		vk::ImageViewType    view;
		uint32_t             layers;
		vk::ImageCreateFlags flags;
	};
	const std::array<Kind, ImageArrays> kinds {{
	    {vk::ImageType::e2D, vk::ImageViewType::e2D, 1, {}},
	    {vk::ImageType::e2D, vk::ImageViewType::e2DArray, 1, {}},
	    {vk::ImageType::e2D, vk::ImageViewType::eCube, 6, vk::ImageCreateFlagBits::eCubeCompatible},
	    {vk::ImageType::e3D, vk::ImageViewType::e3D, 1, {}},
	}};
	for (uint32_t i = 0; i < Placeholders; i++) {
		vk::ImageCreateInfo info {};
		info.flags         = kinds[i % ImageArrays].flags;
		info.imageType     = kinds[i % ImageArrays].type;
		info.format        = vk::Format::eR8G8B8A8Unorm;
		info.extent        = vk::Extent3D {1, 1, 1};
		info.mipLevels     = 1;
		info.arrayLayers   = kinds[i % ImageArrays].layers;
		info.samples       = vk::SampleCountFlagBits::e1;
		info.tiling        = vk::ImageTiling::eOptimal;
		info.usage         = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
		info.initialLayout = vk::ImageLayout::eUndefined;
		EXIT_IF(!m_graphics.CreateImage(info, m_placeholders[i]));
		vk::ImageViewCreateInfo view {};
		view.image            = m_placeholders[i].image;
		view.viewType         = kinds[i % ImageArrays].view;
		view.format           = info.format;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, kinds[i % ImageArrays].layers};
		RequireVulkanSuccess(
		    m_graphics.device.createImageView(&view, nullptr, &m_placeholder_views[i]),
		    "create bindless placeholder view");
	}

	// One fenced submission, before any rendering: clears and a buffer fill are not allowed
	// inside the render passes draws record into.
	vk::CommandPoolCreateInfo pool_info {};
	pool_info.flags            = vk::CommandPoolCreateFlagBits::eTransient;
	pool_info.queueFamilyIndex = m_graphics.queue_family;
	vk::CommandPool pool       = nullptr;
	RequireVulkanSuccess(m_graphics.device.createCommandPool(&pool_info, nullptr, &pool),
	                     "create bindless init pool");
	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = 1;
	vk::CommandBuffer command   = nullptr;
	RequireVulkanSuccess(m_graphics.device.allocateCommandBuffers(&allocate, &command),
	                     "allocate bindless init command buffer");
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	RequireVulkanSuccess(command.begin(&begin), "begin bindless init command buffer");

	std::array<vk::ImageMemoryBarrier, Placeholders> to_transfer {};
	std::array<vk::ImageMemoryBarrier, Placeholders> to_read {};
	for (uint32_t i = 0; i < Placeholders; i++) {
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0,
		                                       kinds[i % ImageArrays].layers};
		to_transfer[i].srcAccessMask       = {};
		to_transfer[i].dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		to_transfer[i].oldLayout           = vk::ImageLayout::eUndefined;
		to_transfer[i].newLayout           = vk::ImageLayout::eTransferDstOptimal;
		to_transfer[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_transfer[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_transfer[i].image               = m_placeholders[i].image;
		to_transfer[i].subresourceRange    = range;
		to_read[i]                         = to_transfer[i];
		to_read[i].srcAccessMask           = vk::AccessFlagBits::eTransferWrite;
		to_read[i].dstAccessMask           = vk::AccessFlagBits::eShaderRead;
		to_read[i].oldLayout               = vk::ImageLayout::eTransferDstOptimal;
		to_read[i].newLayout               = vk::ImageLayout::eShaderReadOnlyOptimal;
	}
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 0, nullptr,
	                        Placeholders, to_transfer.data());
	// Transparent black by default. KYTY_BINDLESS_DEBUG_COLORS=1: slot 0 grey (not resident), slot
	// 1 red (key outside its heap), slot 2 blue (pending), to see which case a surface is in.
	const bool debug_colors = std::getenv("KYTY_BINDLESS_DEBUG_COLORS") != nullptr;
	const auto color        = [debug_colors](float r, float g, float b) {
        return debug_colors ? vk::ClearColorValue {std::array<float, 4> {r, g, b, 1.0f}}
		                           : vk::ClearColorValue {std::array<float, 4> {0.0f, 0.0f, 0.0f, 0.0f}};
	};
	const std::array<vk::ClearColorValue, PlaceholderColors> colors {
	    color(0.5f, 0.5f, 0.5f), color(0.8f, 0.1f, 0.1f), color(0.1f, 0.2f, 0.8f)};
	for (uint32_t i = 0; i < Placeholders; i++) {
		command.clearColorImage(m_placeholders[i].image, vk::ImageLayout::eTransferDstOptimal,
		                        &colors[i / ImageArrays], 1, &to_transfer[i].subresourceRange);
	}
	// translation[0], the entry of keys outside their heap, is the red slot.
	command.fillBuffer(m_translation->Handle(), 4, VK_WHOLE_SIZE, 0u);
	command.fillBuffer(m_translation->Handle(), 0, 4, 1u);
	command.fillBuffer(m_feedback->Handle(), 0, VK_WHOLE_SIZE, 0u);
	vk::BufferMemoryBarrier translation_ready {};
	translation_ready.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	translation_ready.dstAccessMask       = vk::AccessFlagBits::eShaderRead;
	translation_ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	translation_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	translation_ready.buffer              = m_translation->Handle();
	translation_ready.offset              = 0;
	translation_ready.size                = VK_WHOLE_SIZE;
	auto feedback_ready                   = translation_ready;
	feedback_ready.dstAccessMask          = vk::AccessFlagBits::eShaderRead |
	                                        vk::AccessFlagBits::eShaderWrite |
	                                        vk::AccessFlagBits::eHostRead;
	feedback_ready.buffer                 = m_feedback->Handle();
	const std::array<vk::BufferMemoryBarrier, 2> buffers_ready {translation_ready, feedback_ready};
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eAllCommands |
	                            vk::PipelineStageFlagBits::eHost,
	                        {}, 0, nullptr, static_cast<uint32_t>(buffers_ready.size()),
	                        buffers_ready.data(), Placeholders, to_read.data());
	RequireVulkanSuccess(command.end(), "end bindless init command buffer");

	vk::FenceCreateInfo fence_info {};
	vk::Fence           fence = nullptr;
	RequireVulkanSuccess(m_graphics.device.createFence(&fence_info, nullptr, &fence),
	                     "create bindless init fence");
	vk::SubmitInfo submit {};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &command;
	{
		Common::LockGuard lock(m_graphics.queue_mutex);
		RequireVulkanSuccess(m_graphics.queue.submit(1, &submit, fence), "submit bindless init");
	}
	RequireVulkanSuccess(
	    HangTrace::MeasureSyncWait(
	        "gpu-fence", "bindless-init", reinterpret_cast<uint64_t>(static_cast<VkFence>(fence)),
	        0, [&] { return m_graphics.device.waitForFences(1, &fence, VK_TRUE, UINT64_MAX); }),
	    "wait for bindless init");
	m_graphics.device.destroyFence(fence, nullptr);
	m_graphics.device.destroyCommandPool(pool, nullptr);

	std::array<vk::DescriptorImageInfo, Placeholders> infos {};
	std::array<vk::WriteDescriptorSet, Placeholders> writes {};
	for (uint32_t i = 0; i < Placeholders; i++) {
		infos[i]                  = {nullptr, m_placeholder_views[i],
		                             vk::ImageLayout::eShaderReadOnlyOptimal};
		writes[i].dstSet          = m_set;
		writes[i].dstBinding      = Images2D + i % ImageArrays;
		writes[i].dstArrayElement = i / ImageArrays;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eSampledImage;
		writes[i].pImageInfo      = &infos[i];
	}
	m_graphics.device.updateDescriptorSets(Placeholders, writes.data(), 0, nullptr);
}

BindlessTable::~BindlessTable() {
    m_graphics.bindless_enabled = false;
    m_graphics.bindless_layout = nullptr;
    m_graphics.bindless_set = nullptr;
	for (uint32_t i = 0; i < Placeholders; i++) {
		if (m_placeholder_views[i] != nullptr) {
			m_graphics.device.destroyImageView(m_placeholder_views[i], nullptr);
		}
		if (m_placeholders[i].image != nullptr) {
			m_graphics.DeleteImage(m_placeholders[i]);
		}
	}
	m_translation.reset();
	m_feedback.reset();
	if (m_pool != nullptr) {
		m_graphics.device.destroyDescriptorPool(m_pool, nullptr);
	}
	if (m_layout != nullptr) {
		m_graphics.device.destroyDescriptorSetLayout(m_layout, nullptr);
	}
}

bool BindlessTable::AllocateRegion(Heap& heap, uint32_t entries) {
	// Exactly the current bound; growth takes another region, and the old one is retired.
	const auto capacity = entries;
	const auto region   = TakeRegion(capacity);
	if (region == 0) {
		return false;
	}
	const auto old_region  = heap.region;
	const auto old_entries = heap.entries;
	heap.region            = region;
	heap.entries           = capacity;
	heap.slots.resize(capacity, 0u);
	heap.settled.resize(capacity, 0u);
	heap.descriptors.resize(capacity);
	heap.images.resize(capacity);
	heap.memo_hints.resize(capacity);
	heap.fixed_placeholder.resize(capacity, 0u);
	heap.layouts.resize(capacity, vk::ImageLayout::eUndefined);
	heap.ranges.resize(capacity);
	heap.pending_ms.resize(capacity, 0u);
	heap.key_validated.resize(capacity, 0u);
	// Kept keys keep their translation; new keys are pending.
	heap.values.resize(capacity, ShaderRecompiler::IR::BindlessPending);
	if (old_region != 0) {
		RetireRegion(old_region, old_entries);
	}
	WriteRegion(heap);
	LOGF("Bindless heap region: base=0x%016" PRIx64 " binding=%u region=%u entries=%u (was %u at "
	     "%u)\n",
	     heap.base, heap.binding, heap.region, capacity, old_entries, old_region);
	return true;
}

uint32_t BindlessTable::TakeRegion(uint32_t size) {
	for (auto it = m_free_regions.begin(); it != m_free_regions.end(); ++it) {
		if (it->size < size || RegionInUse(it->start)) {
			continue;
		}
		const auto start = it->start;
		if (it->size == size) {
			m_free_regions.erase(it);
		} else {
			// The rest stays free (its commands have completed too).
			it->start += size;
			it->size -= size;
		}
		m_region_ticks.erase(start);
		m_publish_stats.recycled++;
		return start;
	}
	if (size > TranslationEntries - m_next_region) {
		return 0;
	}
	const auto start = m_next_region;
	m_next_region += size;
	return start;
}

void BindlessTable::RetireRegion(uint32_t start, uint32_t size) {
	// A stage prepared but not committed yet may still name the region. Its commit marks the tick
	// it records in (MarkRegionUsed); until then the region waits one tick longer than the
	// current one, in case a submit lands between the preparation and the commit.
	MarkRegionUsed(start, m_scheduler.CurrentTick() + 1u);
	m_free_regions.push_back({start, size});
}

bool BindlessTable::RegionInUse(uint32_t start) {
	const auto found = m_region_ticks.find(start);
	return found != m_region_ticks.end() && !m_scheduler.IsFree(found->second);
}

void BindlessTable::MarkRegionUsed(uint32_t region, uint64_t tick) {
	auto& last = m_region_ticks[region];
	last       = std::max(last, tick);
}

void BindlessTable::WriteRegion(Heap& heap) {
	auto* translation = reinterpret_cast<uint32_t*>(m_translation->Mapped().data());
	auto* feedback    = reinterpret_cast<uint32_t*>(m_feedback->Mapped().data());
	std::memcpy(translation + heap.region, heap.values.data(), heap.entries * sizeof(uint32_t));
	std::memset(feedback + heap.region, 0, heap.entries * sizeof(uint32_t));
	if (!m_translation->IsCoherent()) {
		m_translation->Flush(heap.region * sizeof(uint32_t), heap.entries * sizeof(uint32_t));
	}
	m_feedback->Flush(heap.region * sizeof(uint32_t), heap.entries * sizeof(uint32_t));
	heap.published = heap.values;
	heap.dirty     = false;
}

bool BindlessTable::Publish(Heap& heap) {
	if (!heap.dirty) {
		return true;
	}
	if (heap.values == heap.published) {
		heap.dirty = false;
		m_publish_stats.unchanged++;
		return true;
	}
	const bool in_use = RegionInUse(heap.region);
	if (in_use) {
		if (const auto region = TakeRegion(heap.entries); region != 0) {
			RetireRegion(heap.region, heap.entries);
			heap.region = region;
			WriteRegion(heap);
			m_publish_stats.moved++;
			return true;
		}
		m_publish_stats.full++;
	} else {
		m_publish_stats.in_place++;
	}
	auto*    translation = reinterpret_cast<uint32_t*>(m_translation->Mapped().data());
	uint32_t first       = heap.entries;
	uint32_t last        = 0;
	for (uint32_t key = 0; key < heap.entries; key++) {
		if (heap.values[key] != heap.published[key]) {
			translation[heap.region + key] = heap.values[key];
			heap.published[key]            = heap.values[key];
			first                          = std::min(first, key);
			last                           = key;
		}
	}
	if (!m_translation->IsCoherent() && first <= last) {
		m_translation->Flush((heap.region + first) * sizeof(uint32_t),
		                     (last - first + 1u) * sizeof(uint32_t));
	}
	heap.dirty = false;
	return !in_use;
}

namespace {
// KYTY_BINDLESS_HEAP_INDEX=0 (live, default on): find heaps by scanning every heap, as before the
// place index (A/B only).
Live::Switch g_heap_index("KYTY_BINDLESS_HEAP_INDEX", Live::ParseDefaultOn);
// Idle heaps are swept every HeapSweepInterval new heaps: a heap no lookup returned for
// HeapIdleTicks scheduler ticks (Ghost of Yotei submits about 1300 a second) is released. A full
// translation buffer sweeps at once with HeapIdleTicksWhenFull.
constexpr uint32_t HeapSweepInterval     = 1024;
constexpr uint64_t HeapIdleTicks         = 2048;
constexpr uint64_t HeapIdleTicksWhenFull = 64;
} // namespace

BindlessTable::Heap* BindlessTable::FindOrCreateHeap(
    uint64_t base, uint32_t table_offset, uint32_t record_stride, uint32_t binding, uint32_t entries,
    const ShaderRecompiler::IR::ImageResource& resource) {
	if (entries == 0) {
		return nullptr;
	}
	const auto       now = m_scheduler.CurrentTick();
	const HeapPlace  place {base, table_offset, record_stride, binding};
	const auto found_heap = [&](Heap& heap) -> Heap* {
		if (entries > heap.entries && !AllocateRegion(heap, entries)) {
			return nullptr;
		}
		heap.last_use_tick = now;
		return &heap;
	};
	if (!g_heap_index.On()) {
		for (auto& heap: m_heaps) {
			if (heap.live && heap.base == base && heap.table_offset == table_offset &&
			    heap.record_stride == record_stride && heap.binding == binding &&
			    heap.resource == resource) {
				return found_heap(heap);
			}
		}
	} else if (const auto indexed = m_heap_index.find(place); indexed != m_heap_index.end()) {
		for (auto* heap: indexed->second) {
			// Interpretations sharing a typed array still need independent translations (for
			// example, local cube-coordinate lowering and ordinary 2D-array sampling).
			if (heap->resource == resource) {
				return found_heap(*heap);
			}
		}
	}
	// A new heap. Idle ones go first every so often, and at once when the translation buffer is
	// full.
	if (++m_heaps_since_sweep >= HeapSweepInterval) {
		m_heaps_since_sweep = 0;
		EvictIdleHeaps(HeapIdleTicks);
	}
	Heap* heap = nullptr;
	if (!m_free_heaps.empty()) {
		heap = m_free_heaps.back();
		m_free_heaps.pop_back();
	} else {
		heap = &m_heaps.emplace_back();
	}
	*heap               = Heap {};
	heap->base          = base;
	heap->table_offset  = table_offset;
	heap->record_stride = record_stride;
	heap->binding       = binding;
	heap->resource      = resource;
	heap->last_use_tick = now;
	bool allocated      = AllocateRegion(*heap, entries);
	if (!allocated) {
		EvictIdleHeaps(HeapIdleTicksWhenFull);
		allocated = AllocateRegion(*heap, entries);
	}
	if (!allocated) {
		heap->live = false;
		m_free_heaps.push_back(heap);
		return nullptr;
	}
	m_heap_index[place].push_back(heap);
	return heap;
}

void BindlessTable::EvictIdleHeaps(uint64_t idle_ticks) {
	const auto now = m_scheduler.CurrentTick();
	for (auto& heap: m_heaps) {
		if (heap.live && heap.last_use_tick + idle_ticks < now) {
			EvictHeap(heap);
		}
	}
}

void BindlessTable::EvictHeap(Heap& heap) {
	for (uint32_t key = 0; key < heap.entries; key++) {
		if (heap.images[key]) {
			(void)ReleaseKey(heap, key);
		}
	}
	if (heap.region != 0) {
		RetireRegion(heap.region, heap.entries);
	}
	if (const auto found = m_heap_index.find(
	        HeapPlace {heap.base, heap.table_offset, heap.record_stride, heap.binding});
	    found != m_heap_index.end()) {
		std::erase(found->second, &heap);
		if (found->second.empty()) {
			m_heap_index.erase(found);
		}
	}
	heap      = Heap {};
	heap.live = false;
	m_free_heaps.push_back(&heap);
	m_publish_stats.heaps_evicted++;
}

uint32_t BindlessTable::AllocateSlot(uint32_t binding) {
	if (binding >= ImageArrays) {
		return 0;
	}
	// Released slots in release order: the front is the oldest.
	if (auto& free = m_free_slots[binding]; !free.empty() && m_scheduler.IsFree(free.front().tick)) {
		const auto slot = free.front().slot;
		free.pop_front();
		m_publish_stats.slots_recycled++;
		return slot;
	}
	if (m_next_slot[binding] >= m_images_per_array) {
		m_publish_stats.slot_failures++;
		return 0;
	}
	m_publish_stats.slots_new++;
	return m_next_slot[binding]++;
}

void BindlessTable::ReleaseImageSlots(ImageId id) {
	const auto slots = m_image_slots.find(ImageKey(id));
	if (slots == m_image_slots.end()) {
		return;
	}
	const auto tick = m_scheduler.CurrentTick() + 1u;
	for (const auto& [binding, slot]: slots->second) {
		ForgetSlot(binding, slot);
		if (binding < ImageArrays) {
			m_free_slots[binding].push_back({slot, tick});
			m_publish_stats.slots_released++;
		}
	}
	m_image_slots.erase(slots);
}

void BindlessTable::WriteSlot(uint32_t binding, uint32_t slot, vk::ImageView view,
                              vk::ImageLayout layout) {
	const vk::DescriptorImageInfo info {nullptr, view, layout};
	vk::WriteDescriptorSet        write {};
	write.dstSet          = m_set;
	write.dstBinding      = binding;
	write.dstArrayElement = slot;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eSampledImage;
	write.pImageInfo      = &info;
	m_graphics.device.updateDescriptorSets(1, &write, 0, nullptr);
	if (binding >= ImageArrays) {
		return;
	}
	auto& views = m_slot_views[binding];
	if (views.size() <= slot) {
		views.resize(slot + 1u);
	}
	if (const auto old = views[slot]; old != nullptr) {
		if (const auto found = m_view_slots.find(static_cast<VkImageView>(old));
		    found != m_view_slots.end()) {
			std::erase(found->second, std::pair {binding, slot});
			if (found->second.empty()) {
				m_view_slots.erase(found);
			}
		}
	}
	const bool placeholder =
	    std::ranges::find(m_placeholder_views, view) != m_placeholder_views.end();
	views[slot] = placeholder ? vk::ImageView {} : view;
	if (!placeholder && view != nullptr) {
		m_view_slots[static_cast<VkImageView>(view)].emplace_back(binding, slot);
	}
}

void BindlessTable::SetTranslation(Heap& heap, uint32_t key, uint32_t slot) {
	if (heap.values[key] != slot) {
		heap.values[key] = slot;
		heap.dirty       = true;
	}
}

void BindlessTable::AddImageReference(ImageId id, Heap& heap, uint32_t key) {
	m_image_refs[ImageKey(id)].emplace_back(&heap, key);
}

bool BindlessTable::ReleaseKey(Heap& heap, uint32_t key) {
	heap.key_validated[key] = 0;
	const auto id      = heap.images[key];
	bool       no_refs = false;
	if (id) {
		const auto found = m_image_refs.find(ImageKey(id));
		if (found != m_image_refs.end()) {
			std::erase(found->second, std::pair<Heap*, uint32_t> {&heap, key});
			const bool in_heap = std::any_of(found->second.begin(), found->second.end(),
			                                 [&](const auto& ref) { return ref.first == &heap; });
			if (!in_heap) {
				std::erase(heap.resolved, id);
			}
			if (found->second.empty()) {
				m_image_refs.erase(found);
				no_refs = true;
			}
		}
	}
	if (no_refs) {
		// No key names the image any more: its slots go back to the free list. A key that names
		// it again takes a slot anew (FindSlot no longer finds these).
		ReleaseImageSlots(id);
	}
	heap.slots[key]   = 0;
	heap.settled[key] = 0;
	heap.images[key]  = {};
	SetTranslation(heap, key, ShaderRecompiler::IR::BindlessPending);
	return no_refs;
}

void BindlessTable::QueueUnregistered(ImageId id) {
    if (!Enabled()) return;
    std::scoped_lock lock(m_retirement_mutex);
    m_unregistered.push_back(id);
}

void BindlessTable::ApplyUnregistered() {
    std::vector<ImageId> ids;
    { std::scoped_lock lock(m_retirement_mutex); ids.swap(m_unregistered); }
    for (const auto id: ids) OnImageUnregistered(id);
}

void BindlessTable::ForgetSlot(uint32_t binding, uint32_t slot) {
	if (binding >= ImageArrays || slot >= m_slot_views[binding].size()) {
		return;
	}
	auto& view = m_slot_views[binding][slot];
	if (view == nullptr) {
		return;
	}
	Image::NoteBindingStateChange(Image::BindingChange::BindlessSlot);
	if (const auto found = m_view_slots.find(static_cast<VkImageView>(view));
	    found != m_view_slots.end()) {
		std::erase(found->second, std::pair {binding, slot});
		if (found->second.empty()) {
			m_view_slots.erase(found);
		}
	}
	view = nullptr;
}

uint32_t BindlessTable::FindSlot(uint32_t binding, vk::ImageView view) const {
    const auto found = m_view_slots.find(static_cast<VkImageView>(view));
    if (found != m_view_slots.end()) for (const auto& [array, slot]: found->second)
        if (array == binding) return slot;
    return 0;
}

void BindlessTable::AddSlotOwner(ImageId id, uint32_t binding, uint32_t slot) {
    m_image_slots[ImageKey(id)].emplace_back(binding, slot);
}

void BindlessTable::OnImageUnregistered(ImageId id) {
    Image::NoteBindingStateChange(Image::BindingChange::BindlessUnregister);
    // Keep ownership even after the last key moved elsewhere. Otherwise a recycled raw view
    // handle could match a stale slot whose Vulkan descriptor still names the retired object.
    ReleaseImageSlots(id);
    if (const auto found = m_image_refs.find(ImageKey(id)); found != m_image_refs.end()) {
        for (const auto& [heap, key]: found->second) {
            heap->slots[key] = 0;
            heap->settled[key] = 0;
            heap->images[key] = {};
            SetTranslation(*heap, key, 0u);
        }
        m_image_refs.erase(found);
    }
}

} // namespace Libs::Graphics
