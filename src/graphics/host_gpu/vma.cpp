#include "graphics/host_gpu/vulkanCommon.h"
#include "common/environment.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define VMA_IMPLEMENTATION
#include <cstdio>
#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace Libs::Graphics {

namespace {

// Images the texture cache released, kept for an identical CreateImage (KYTY_IMAGE_POOL_MB,
// default 256; 0 = off). The cache frees images under pressure and creates the same shapes again
// a moment later; each creation was an allocation in the driver. A pooled image comes back as a
// new one: its contents and layout are undefined, which is what CreateImage reports anyway
// (initial layout undefined), and VMA's resource-aliasing rules ask for no more. Only images the
// owner released once the GPU was done with them (VulkanImage::release_to_pool) are kept; none
// while memory is short (GraphicContext::MemoryShortfall), and none older than
// KYTY_IMAGE_POOL_AGE_S (default 10).
struct ImagePoolKey {
	vk::Format          format  = vk::Format::eUndefined;
	vk::ImageType       type    = vk::ImageType::e2D;
	uint32_t            width   = 0;
	uint32_t            height  = 0;
	uint32_t            depth   = 0;
	uint32_t            layers  = 0;
	uint32_t            mips    = 0;
	uint32_t            samples = 0;
	VkImageUsageFlags   usage   = 0;
	VkImageCreateFlags  flags   = 0;
	bool operator==(const ImagePoolKey&) const = default;
};

struct ImagePoolKeyHash {
	size_t operator()(const ImagePoolKey& key) const noexcept {
		uint64_t hash = 0xcbf29ce484222325ull;
		for (const uint64_t value: {static_cast<uint64_t>(key.format), static_cast<uint64_t>(key.type),
		                            uint64_t {key.width}, uint64_t {key.height}, uint64_t {key.depth},
		                            uint64_t {key.layers}, uint64_t {key.mips}, uint64_t {key.samples},
		                            uint64_t {key.usage}, uint64_t {key.flags}}) {
			hash = (hash ^ value) * 0x100000001b3ull;
		}
		return static_cast<size_t>(hash);
	}
};

struct PooledImage {
	VkImage                               image      = VK_NULL_HANDLE;
	VmaAllocation                         allocation = nullptr;
	uint64_t                              bytes      = 0;
	std::chrono::steady_clock::time_point released;
};

std::mutex                                                            g_image_pool_mutex;
std::unordered_multimap<ImagePoolKey, PooledImage, ImagePoolKeyHash> g_image_pool;
uint64_t                                                              g_image_pool_bytes = 0;

uint64_t ImagePoolLimit() {
	static const uint64_t limit = [] {
		const char* value = std::getenv("KYTY_IMAGE_POOL_MB");
		return (value != nullptr ? std::strtoull(value, nullptr, 10) : uint64_t {256}) << 20u;
	}();
	return limit;
}

std::chrono::seconds ImagePoolAge() {
	static const std::chrono::seconds age = [] {
		const char* value = std::getenv("KYTY_IMAGE_POOL_AGE_S");
		return std::chrono::seconds(value != nullptr ? std::strtoull(value, nullptr, 10) : 10u);
	}();
	return age;
}

ImagePoolKey PoolKey(const vk::ImageCreateInfo& info) {
	return {info.format,
	        info.imageType,
	        info.extent.width,
	        info.extent.height,
	        info.extent.depth,
	        info.arrayLayers,
	        info.mipLevels,
	        static_cast<uint32_t>(info.samples),
	        static_cast<VkImageUsageFlags>(info.usage),
	        static_cast<VkImageCreateFlags>(info.flags)};
}

ImagePoolKey PoolKey(const VulkanImage& image) {
	return {image.format,
	        image.image_type,
	        image.extent.width,
	        image.extent.height,
	        image.extent.depth,
	        image.layers,
	        image.mip_levels,
	        image.samples,
	        static_cast<VkImageUsageFlags>(image.usage),
	        static_cast<VkImageCreateFlags>(image.flags)};
}

// Destroys the pooled images older than the pool age, or all of them. Holds g_image_pool_mutex.
void TrimImagePool(VmaAllocator allocator, bool all) {
	if (g_image_pool.empty()) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	for (auto it = g_image_pool.begin(); it != g_image_pool.end();) {
		if (all || now - it->second.released > ImagePoolAge()) {
			vmaDestroyImage(allocator, it->second.image, it->second.allocation);
			g_image_pool_bytes -= it->second.bytes;
			it = g_image_pool.erase(it);
		} else {
			++it;
		}
	}
}

} // namespace

bool GraphicContext::CreateAllocator() {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(instance == nullptr || physical_device == nullptr || device == nullptr ||
	        allocator != nullptr);

	VmaVulkanFunctions functions {};
	functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo info {};
	info.instance         = instance;
	info.physicalDevice   = physical_device;
	info.device           = device;
	info.pVulkanFunctions = &functions;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (memory_budget_ext_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	}

	// Testing aid: KYTY_VRAM_LIMIT_MB=<n> caps every device-local heap at n MB, so a large card
	// behaves like a 4, 6 or 8 GB one: VMA reports the capped budget, allocations past it fail
	// or spill as they would there, and the caches' collection thresholds follow the budget.
	std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> heap_limits {};
	if (const char* value = std::getenv("KYTY_VRAM_LIMIT_MB"); value != nullptr) {
		const auto  limit      = static_cast<VkDeviceSize>(std::strtoull(value, nullptr, 10)) << 20u;
		const auto& properties = GetPhysicalDeviceMemoryProperties();
		if (limit != 0) {
			for (uint32_t heap = 0; heap < properties.memoryHeapCount; heap++) {
				const bool device_local = static_cast<bool>(properties.memoryHeaps[heap].flags &
				                                            vk::MemoryHeapFlagBits::eDeviceLocal);
				heap_limits[heap] = device_local ? std::min<VkDeviceSize>(
				                                       limit, properties.memoryHeaps[heap].size)
				                                 : VK_WHOLE_SIZE;
				if (device_local) {
					LOGF("KYTY_VRAM_LIMIT_MB: device heap %u capped at %" PRIu64 " MB (of %" PRIu64
					     " MB)\n",
					     heap, static_cast<uint64_t>(heap_limits[heap] >> 20u),
					     static_cast<uint64_t>(properties.memoryHeaps[heap].size >> 20u));
				}
			}
			info.pHeapSizeLimit = heap_limits.data();
		}
	}

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &allocator));
	if (result != vk::Result::eSuccess) {
		LOGF("vmaCreateAllocator failed: %s\n", vk::to_string(result).c_str());
		return false;
	}

	// A system whose commit limit is nearly used up stops the session later with "Out of memory"
	// (ISSUES #21); say so at start. KYTY_COMMIT_WARN_MB sets the threshold (default 16 GB).
	uint64_t commit_limit = 0;
	uint64_t commit_free  = 0;
	if (Common::VirtualMemory::QueryCommit(commit_limit, commit_free)) {
		const char*    value     = std::getenv("KYTY_COMMIT_WARN_MB");
		const uint64_t warn_mb   = value != nullptr ? std::strtoull(value, nullptr, 10) : 16384u;
		LOGF("Commit limit (start): %" PRIu64 " MB free of %" PRIu64 " MB\n", commit_free >> 20u,
		     commit_limit >> 20u);
		if ((commit_free >> 20u) < warn_mb) {
			std::fprintf(stderr,
			             "Warning: only %" PRIu64 " MB of the system's commit limit (%" PRIu64
			             " MB, RAM plus page file) are free; a game can need 16-24 GB. If the "
			             "emulator stops with \"Out of memory\", close other programs or enlarge "
			             "the page file.\n",
			             commit_free >> 20u, commit_limit >> 20u);
			LOGF("Warning: only %" PRIu64 " MB of the commit limit are free (threshold %" PRIu64
			     " MB)\n",
			     commit_free >> 20u, warn_mb);
		}
	}
	return true;
}

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	{
		std::lock_guard lock(g_image_pool_mutex);
		TrimImagePool(allocator, true);
	}
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
}

// The system's commit limit (Windows: RAM + page file): an allocation fails when it is used up,
// whatever the VMA budget says (ISSUES #21: one Wolverine session commits ~23 GB).
static void LogCommit(const char* when) {
	uint64_t limit     = 0;
	uint64_t available = 0;
	if (Common::VirtualMemory::QueryCommit(limit, available)) {
		LOGF("Commit limit (%s): %" PRIu64 " MB free of %" PRIu64 " MB\n", when, available >> 20u,
		     limit >> 20u);
	}
}

// The video memory the emulator plans for, whatever the card has (KYTY_VRAM_TARGET_MB; 0, the
// default, = the card's own budget; Senaxx's 14336 = what a 16 GB card leaves an application). On
// a 24 GB card the 14 GB target had the caches collecting from 90 % of it on: in the Trask
// compound (~19 GB in use) the buffer collector ran ~1,900 times per 5 s, waited for the GPU
// 1.3-1.6 s of every 5 and the readback ring ~1.9 s: 2-4 fps; with the card's budget 12-22 fps
// (2026-10-20). On cards with 16 GB or less the card's budget is the smaller one anyway.
static uint64_t VramTargetBytes() {
	static const uint64_t target = [] {
		const char* value = std::getenv("KYTY_VRAM_TARGET_MB");
		return (value != nullptr ? std::strtoull(value, nullptr, 10) : uint64_t {0}) << 20u;
	}();
	return target;
}

uint64_t GraphicContext::MemoryShortfall() const {
	// KYTY_COMMIT_RESERVE_MB (default 0 = off): collecting video memory for a commit shortfall
	// was tried on 2026-10-09 and withdrawn. On a PC whose commit limit is short for other reasons
	// (other programs, guest memory, host heaps) it never catches up: the collectors wrote back
	// and freed ~100 images per interval from 7 GB of device use on, the frame rate fell to 7 fps
	// and glyph atlases lost letters. The limit is also dynamic on a system-managed page file. The
	// start-up warning and the log lines remain; the video memory target is the pressure that counts.
	static const uint64_t reserve = [] {
		const char* value = std::getenv("KYTY_COMMIT_RESERVE_MB");
		return (value != nullptr ? std::strtoull(value, nullptr, 10) : uint64_t {0}) << 20u;
	}();
	static std::atomic<int64_t>  next_ms {0};
	static std::atomic<uint64_t> shortfall {0};
	const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
	                     std::chrono::steady_clock::now().time_since_epoch())
	                     .count();
	auto next = next_ms.load(std::memory_order_relaxed);
	if (now >= next && next_ms.compare_exchange_strong(next, now + 250, std::memory_order_relaxed)) {
		// The system's commit limit below the reserve.
		uint64_t limit     = 0;
		uint64_t available = 0;
		const uint64_t commit =
		    reserve != 0 && Common::VirtualMemory::QueryCommit(limit, available) &&
		            available < reserve
		        ? reserve - available
		        : 0;
		// Everything the device holds (images, buffers, the driver's own) past 90 % of the
		// budget, which the video memory target caps: the caches' own thresholds leave out the
		// bindless heap textures and render targets, so they alone cannot keep the total in it.
		// With hysteresis (KYTY_VRAM_HYSTERESIS, default on): past KYTY_VRAM_HIGH_PCT (90) of the
		// budget the caches collect down to KYTY_VRAM_LOW_PCT (82), then stop until the high mark
		// is crossed again. Collecting only the excess over one mark kept the device on it: every
		// new allocation crossed it again, and the caches freed and created the same resources over
		// and over (2-4 fps late in a Wolverine route). =0 collects the excess over 90 % as before.
		static const bool hysteresis = [] {
			const char* value = std::getenv("KYTY_VRAM_HYSTERESIS");
			return value == nullptr || value[0] != '0';
		}();
		static const uint64_t high_pct = [] {
			const char* value = std::getenv("KYTY_VRAM_HIGH_PCT");
			return std::clamp<uint64_t>(value != nullptr ? std::strtoull(value, nullptr, 10) : 90, 50, 98);
		}();
		static const uint64_t low_pct = [] {
			const char* value = std::getenv("KYTY_VRAM_LOW_PCT");
			return std::clamp<uint64_t>(value != nullptr ? std::strtoull(value, nullptr, 10) : 82, 40,
			                            high_pct);
		}();
		static std::atomic<bool> collecting {false};
		uint64_t device = 0;
		if (CanReportMemoryUsage()) {
			const auto budget = GetTotalMemoryBudget();
			const auto usage  = GetDeviceMemoryUsage();
			if (!hysteresis) {
				const auto mark = budget / 10 * 9;
				device          = budget != 0 && usage > mark ? usage - mark : 0;
			} else if (budget != 0) {
				const auto high = budget / 100 * high_pct;
				const auto low  = budget / 100 * low_pct;
				if (usage > high) {
					collecting.store(true, std::memory_order_relaxed);
				} else if (usage <= low) {
					collecting.store(false, std::memory_order_relaxed);
				}
				device = collecting.load(std::memory_order_relaxed) && usage > low ? usage - low : 0;
			}
		}
		// KYTY_MEMORY_STATS=1 (log only): every 10 s, the process's RAM and the device heaps, to
		// measure RAM and VRAM use in normal play (LogCommit and the collectors' lines are LOGF).
		static const bool        memory_stats = Common::Environment::IsSet("KYTY_MEMORY_STATS");
		static std::atomic<int64_t> next_stats_ms {0};
		if (auto due = next_stats_ms.load(std::memory_order_relaxed);
		    memory_stats && now >= due &&
		    next_stats_ms.compare_exchange_strong(due, now + 10000, std::memory_order_relaxed)) {
			uint64_t resident = 0, private_bytes = 0, commit_limit = 0, commit_free = 0;
			(void)Common::VirtualMemory::QueryProcessMemory(resident, private_bytes);
			(void)Common::VirtualMemory::QueryCommit(commit_limit, commit_free);
			std::fprintf(stderr,
			             "[memory] RAM: working set %" PRIu64 " MB, private %" PRIu64
			             " MB; system commit %" PRIu64 " MB free of %" PRIu64 " MB; device %" PRIu64
			             " MB of %" PRIu64 " MB budget, collecting=%d\n",
			             resident >> 20u, private_bytes >> 20u, commit_free >> 20u,
			             commit_limit >> 20u, GetDeviceMemoryUsage() >> 20u,
			             GetTotalMemoryBudget() >> 20u,
			             collecting.load(std::memory_order_relaxed) ? 1 : 0);
		}
		const uint64_t value    = std::max(commit, device);
		const auto     previous = shortfall.exchange(value, std::memory_order_relaxed);
		static std::atomic<uint32_t> reported {0};
		if ((previous == 0) != (value == 0) && reported.fetch_add(1) < 64) {
			LOGF("Memory: %s (commit %" PRIu64 " MB free of %" PRIu64 " MB, reserve %" PRIu64
			     " MB; device %" PRIu64 " MB of %" PRIu64 " MB budget)\n",
			     value != 0 ? "short, the caches collect" : "room again", available >> 20u,
			     limit >> 20u, reserve >> 20u, GetDeviceMemoryUsage() >> 20u,
			     GetTotalMemoryBudget() >> 20u);
		}
	}
	return shortfall.load(std::memory_order_relaxed);
}

void GraphicContext::LogMemoryBudget() const {
	LogCommit("now");
	if (allocator == nullptr || physical_device == nullptr) {
		return;
	}

	const auto& properties = GetPhysicalDeviceMemoryProperties();
	VmaBudget   budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		// stderr: called on the way to an out-of-memory exit, whose message points at these lines.
		std::fprintf(stderr,
		             "VMA heap %u: usage=%" PRIu64 " MB, budget=%" PRIu64 " MB, allocation=%" PRIu64
		             " MB, blocks=%" PRIu64 " MB, size=%" PRIu64 " MB\n",
		             i, static_cast<uint64_t>(budgets[i].usage) >> 20u,
		             static_cast<uint64_t>(budgets[i].budget) >> 20u,
		             static_cast<uint64_t>(budgets[i].statistics.allocationBytes) >> 20u,
		             static_cast<uint64_t>(budgets[i].statistics.blockBytes) >> 20u,
		             static_cast<uint64_t>(properties.memoryHeaps[i].size) >> 20u);
	}
}

uint64_t GraphicContext::GetDeviceMemoryUsage() const {
	if (!CanReportMemoryUsage() || allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t usage = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const bool device_local =
		    static_cast<bool>(physical_device_memory_properties.memoryHeaps[heap].flags &
		                      vk::MemoryHeapFlagBits::eDeviceLocal);
		if (!discrete || device_local) {
			usage += budgets[heap].usage;
		}
	}
	return usage;
}

uint64_t GraphicContext::GetTotalMemoryBudget() const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t budget = 0;
	uint64_t local  = 0;
	uint64_t usage  = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		const bool  device_local =
		    static_cast<bool>(properties.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		if (device_local) {
			local += properties.size;
		}
		if (!discrete || device_local) {
			budget += CanReportMemoryUsage() ? budgets[heap].budget : properties.size;
			usage += CanReportMemoryUsage() ? budgets[heap].usage : 0;
		}
	}
	if (discrete) {
		const auto own = budget - std::min<uint64_t>(budget / 8, 1024ull * 1024 * 1024);
		const auto target = VramTargetBytes();
		return target != 0 ? std::min(own, target) : own;
	}
	constexpr uint64_t system_reserve = 8ull * 1024 * 1024 * 1024;
	const auto         available      = budget > usage ? budget - usage : uint64_t {0};
	return std::max(local, available > system_reserve ? available - system_reserve : uint64_t {0});
}

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.allocation != nullptr);

	const bool plain = image_info.pNext == nullptr && image_info.tiling == vk::ImageTiling::eOptimal &&
	                   image_info.initialLayout == vk::ImageLayout::eUndefined &&
	                   image_info.sharingMode == vk::SharingMode::eExclusive;
	vk::Image::CType native_image = VK_NULL_HANDLE;
	auto             result       = vk::Result::eErrorOutOfDeviceMemory;
	if (plain && ImagePoolLimit() != 0) {
		std::lock_guard lock(g_image_pool_mutex);
		TrimImagePool(allocator, false);
		if (const auto found = g_image_pool.find(PoolKey(image_info)); found != g_image_pool.end()) {
			native_image     = found->second.image;
			image.allocation = found->second.allocation;
			g_image_pool_bytes -= found->second.bytes;
			g_image_pool.erase(found);
			result = vk::Result::eSuccess;
		}
	}

	// KYTY_IMAGE_WITHIN_BUDGET (default on; =0 as before): an image only sampled is created within
	// the device budget, and past it goes to host memory below. Created past the budget, VMA leaves
	// the decision to the driver, which on Windows pages the device memory in and out while the
	// GPU waits (VMA: within-budget is for non-essential resources; render targets and storage
	// images keep the old rule).
	static const bool within_budget = [] {
		const char* value = std::getenv("KYTY_IMAGE_WITHIN_BUDGET");
		return value == nullptr || value[0] != '0';
	}();
	constexpr auto TargetUsage = vk::ImageUsageFlagBits::eColorAttachment |
	                             vk::ImageUsageFlagBits::eDepthStencilAttachment |
	                             vk::ImageUsageFlagBits::eStorage;
	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if (within_budget && !(image_info.usage & TargetUsage)) {
		alloc_info.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
	}
	if (result != vk::Result::eSuccess) {
		result = static_cast<vk::Result>(
		    vmaCreateImage(allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
		                   &alloc_info, &native_image, &image.allocation, nullptr));
	}
	if (result != vk::Result::eSuccess && ImagePoolLimit() != 0) {
		// Released images kept for reuse hold up to KYTY_IMAGE_POOL_MB of video memory: give it
		// back before this image goes to system memory (the collectors only empty the pool once
		// they see the shortfall, a moment later).
		bool trimmed = false;
		{
			std::lock_guard lock(g_image_pool_mutex);
			trimmed = !g_image_pool.empty();
			TrimImagePool(allocator, true);
		}
		if (trimmed) {
			result = static_cast<vk::Result>(vmaCreateImage(
			    allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
			    &alloc_info, &native_image, &image.allocation, nullptr));
		}
	}
	if (result != vk::Result::eSuccess) {
		// The device heap is full. The guest has a unified 16 GB pool, so the image is
		// real; place it in host memory and pay in bandwidth rather than fail.
		alloc_info.flags          = 0;
		alloc_info.requiredFlags  = 0;
		alloc_info.preferredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
		result                    = static_cast<vk::Result>(vmaCreateImage(
            allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
            &alloc_info, &native_image, &image.allocation, nullptr));
		static std::atomic<uint32_t> spill_count {0};
		if (const auto seen = spill_count.fetch_add(1, std::memory_order_relaxed); seen < 32) {
			LOGF("Image spilled to host memory (%u): %ux%ux%u layers=%u levels=%u format=%d -> %s\n",
			     seen + 1, image_info.extent.width, image_info.extent.height,
			     image_info.extent.depth, image_info.arrayLayers, image_info.mipLevels,
			     static_cast<int>(image_info.format), vk::to_string(result).c_str());
		}
	}
	image.image = native_image;
	if (result != vk::Result::eSuccess) {
		LogMemoryBudget();
		return false;
	}

	image.format     = image_info.format;
	image.image_type = image_info.imageType;
	image.extent     = image_info.extent;
	image.layers     = image_info.arrayLayers;
	image.mip_levels = image_info.mipLevels;
	image.samples    = static_cast<uint32_t>(image_info.samples);
	image.usage      = image_info.usage;
	image.flags      = image_info.flags;
	image.state      = {.layout = image_info.initialLayout};
	image.subresource_states.clear();
	VkMemoryPropertyFlags memory_flags = 0;
	vmaGetAllocationMemoryProperties(allocator, image.allocation, &memory_flags);
	image.poolable        = plain && (memory_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
	image.release_to_pool = false;

	return true;
}

void GraphicContext::DeleteImage(VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image == nullptr || image.allocation == nullptr);

	if (image.poolable && image.release_to_pool && ImagePoolLimit() != 0) {
		VmaAllocationInfo info {};
		vmaGetAllocationInfo(allocator, image.allocation, &info);
		const bool short_of_memory = MemoryShortfall() != 0;
		std::lock_guard lock(g_image_pool_mutex);
		TrimImagePool(allocator, short_of_memory);
		if (!short_of_memory && g_image_pool_bytes + info.size <= ImagePoolLimit()) {
			g_image_pool.emplace(PoolKey(image), PooledImage {image.image, image.allocation, info.size,
			                                                  std::chrono::steady_clock::now()});
			g_image_pool_bytes += info.size;
			image.image      = nullptr;
			image.allocation = nullptr;
			return;
		}
	}
	vmaDestroyImage(allocator, image.image, image.allocation);
	image.image      = nullptr;
	image.allocation = nullptr;
}

} // namespace Libs::Graphics
