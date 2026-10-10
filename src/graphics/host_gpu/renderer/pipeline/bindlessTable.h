#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDLESSTABLE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDLESSTABLE_H_

#include "common/common.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/pipeline/bindlessLimits.h"
#include "graphics/shader/recompiler/ir/BindlessBindings.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class SamplerCache;

// Descriptor set 1 of every pipeline that samples bindless images: typed arrays of sampled
// images a shader indexes with a slot it looks up per pixel, the key -> slot translation the
// host fills per guest descriptor heap, and a bitset where shaders flag keys whose texture is
// not resident yet. Created only when the device supports descriptor indexing.
class BindlessTable {
public:
	enum Binding : uint32_t {
		Images2D      = 0,
		Images2DArray = 1,
		ImagesCube    = 2,
		Images3D      = 3,
		Translation   = 4,
		Feedback      = 5,
		Samplers      = 6,
	};
	static_assert(Images2D == ShaderRecompiler::IR::BindlessImages2D &&
	              Images2DArray == ShaderRecompiler::IR::BindlessImages2DArray &&
	              ImagesCube == ShaderRecompiler::IR::BindlessImagesCube &&
	              Images3D == ShaderRecompiler::IR::BindlessImages3D &&
	              Translation == ShaderRecompiler::IR::BindlessTranslation &&
	              Feedback == ShaderRecompiler::IR::BindlessFeedback &&
	              Samplers == ShaderRecompiler::IR::BindlessSamplers);
	static constexpr uint32_t ImageArrays        = 4;
	static constexpr uint32_t PlaceholderColors  = 3;
	static constexpr uint32_t Placeholders       = ImageArrays * PlaceholderColors;
	static constexpr uint32_t MaxImagesPerArray  = BindlessMaxImagesPerArray;
	static constexpr uint32_t TranslationEntries = 1u << 20u;
	static constexpr uint32_t MaxSamplers        = 4096;

	BindlessTable(GraphicContext& graphics, CommandScheduler& scheduler);
	~BindlessTable();
	KYTY_CLASS_NO_COPY(BindlessTable);

	[[nodiscard]] bool                    Enabled() const noexcept { return m_set != nullptr; }
	[[nodiscard]] vk::DescriptorSetLayout Layout() const noexcept { return m_layout; }
	[[nodiscard]] vk::DescriptorSet       Set() const noexcept { return m_set; }
	[[nodiscard]] uint32_t                ImagesPerArray() const noexcept { return m_images_per_array; }

	// A guest descriptor heap as one view type samples it: a region of the translation (and
	// feedback) buffer, one entry per key, and the textures resolved so far.
	struct Heap {
		uint64_t                            base         = 0;
		uint32_t                            table_offset = 0;
		uint32_t                            record_stride = 32; // bytes between keys' T#s
		uint32_t                            binding      = 0;
		uint32_t                            region       = 0;
		// Keys the region holds. The guest appends textures to a heap as it streams them in and
		// its descriptor grows; each draw patches its own descriptor's entry count.
		uint32_t                            entries      = 0;
		ShaderRecompiler::IR::ImageResource resource;
		std::vector<uint32_t>               slots;    // per key; 0 = not resolved
		std::vector<uint8_t>                settled;  // per key; resolved, or known placeholder
		std::vector<ImageId>                resolved; // images to keep readable for draws
		// Per settled key, the T# it was settled from and the image it resolved to (none for a
		// placeholder). The guest rewrites entries as it streams textures out and others in;
		// Every consumer rereads the complete current heap from certified clean backing.
		std::vector<std::array<uint32_t, 8>> descriptors;
		std::vector<ImageId>                 images;
		// Per key, the texture binding memo's hash and tag of its last resolution (tag 0: none).
		// A consumer that finds the same T# passes them as hints: no hashing, no key comparison.
		struct MemoHint {
			uint64_t hash = 0;
			uint64_t tag  = 0;
		};
		std::vector<MemoHint> memo_hints;
		// Per settled key: a placeholder its T# alone decides (incompatible or null descriptor,
		// another view type), and, for a resolved key, the layout and subresource range the stage
		// commit makes readable. A consumer that finds the heap unchanged repeats them
		// (RenderExecutor::RepeatBindlessHeap).
		std::vector<uint8_t>               fixed_placeholder;
		std::vector<vk::ImageLayout>       layouts;
		std::vector<ImageSubresourceRange> ranges;
		// Per key, the translation the host wants (SetTranslation) and the one the region holds
		// (Publish writes the difference). `dirty`: values changed since the last Publish.
		std::vector<uint32_t> values;
		std::vector<uint32_t> published;
		bool                  dirty = false;
		// Per key settled as a pending placeholder (no fixed answer from its T#): when, in
		// steady-clock milliseconds (KYTY_BINDLESS_PENDING_RETRY_MS).
		std::vector<uint64_t> pending_ms;
		// KYTY_BINDLESS_TICK_REPEAT: per key, the scheduler tick in which its memo repeat checks
		// (TextureBindingMemo::TryRepeatKeys / TryRepeatEachKey) last passed, at Image::
		// BindingStateGeneration() == validated_generation (0: not in that generation). A later
		// consumer in the same tick at the same generation skips them for that key: nothing that
		// could fail them changed, and their LRU touches are per tick already.
		std::vector<uint64_t> key_validated;
		uint64_t              validated_generation = 0;
		// The scheduler tick of the last FindOrCreateHeap that returned it; false once evicted
		// (EvictIdleHeaps), when the object waits in the free list for another heap.
		uint64_t last_use_tick = 0;
		bool     live          = true;
	};

	// The heap for (base, table offset, view binding, complete resource interpretation), created
	// with every entry pending and
	// room to grow, or moved to a larger region when a descriptor covers more keys; null when
	// the translation buffer is full.
	[[nodiscard]] Heap* FindOrCreateHeap(uint64_t base, uint32_t table_offset,
	                                     uint32_t record_stride, uint32_t binding, uint32_t entries,
	                                     const ShaderRecompiler::IR::ImageResource& resource);
	[[nodiscard]] std::deque<Heap>& Heaps() noexcept { return m_heaps; }
	// Heaps, their distinct places, and sampler heaps (the "Bindless heaps 10s" line).
	struct HeapCounts {
		size_t heaps = 0, places = 0, sampler_heaps = 0;
	};
	[[nodiscard]] HeapCounts CountHeaps() const noexcept {
		return {m_heaps.size() - m_free_heaps.size(), m_heap_index.size(), m_sampler_heaps.size()};
	}
	// A slot no command that may still run reads: one released earlier (its image lost its last
	// key, or was retired) once the commands recorded up to then completed, else a new one; 0
	// when every slot is in use.
	[[nodiscard]] uint32_t AllocateSlot(uint32_t binding);
	void WriteSlot(uint32_t binding, uint32_t slot, vk::ImageView view, vk::ImageLayout layout);
	// Resolve a key to a slot (0 = placeholder), or back to pending. Only the host copy changes;
	// Publish makes it visible to the commands recorded after it.
	void SetTranslation(Heap& heap, uint32_t key, uint32_t slot);
	// Writes the heap's changed translations. When commands that have not completed may read the
	// heap's region (MarkRegionUsed), the whole translation moves to another region instead (copy
	// on write), so those commands keep reading what they were recorded with; the old region is
	// reused once they complete. With no region free the region is written in place: memory-safe
	// (every value names a live view or the placeholder), but in-flight commands may then sample
	// the new texture of a key. False in that case.
	bool Publish(Heap& heap);
	// Commands recorded up to `tick` read the translation region that starts at `region`.
	void MarkRegionUsed(uint32_t region, uint64_t tick);
	// Publish outcomes since the last call (the "Bindless heaps" line).
	struct PublishStats {
		uint64_t moved     = 0; // copy on write to another region
		uint64_t in_place  = 0; // changed entries written into an idle region
		uint64_t unchanged = 0; // dirty, but every value equals the published one
		uint64_t full      = 0; // in place although in use: no region free
		uint64_t recycled  = 0; // regions taken from the free list
		uint64_t slots_recycled = 0; // image slots taken from the free list
		uint64_t slots_new      = 0; // image slots never used before
		uint64_t slot_failures  = 0; // AllocateSlot found no slot (the key samples the placeholder)
		uint64_t slots_released = 0; // image slots released (no key left, or the image retired)
		uint64_t heaps_evicted  = 0; // idle heaps whose region, keys and slots were released
	};
	[[nodiscard]] PublishStats TakePublishStats() noexcept { return std::exchange(m_publish_stats, {}); }
	// Releases every heap no lookup returned for more than `idle_ticks` scheduler ticks: its keys
	// (and the image slots only they held), its region (reusable once the GPU is done with it) and
	// its place in the index. Guest descriptor tables move (Ghost of Yotei builds tens of
	// thousands at new addresses), so heaps that are never released fill the translation buffer.
	// FindOrCreateHeap calls it; public for tests.
	void EvictIdleHeaps(uint64_t idle_ticks);
	void AddImageReference(ImageId id, Heap& heap, uint32_t key);
    void AddSlotOwner(ImageId id, uint32_t binding, uint32_t slot);
	// The key no longer samples what it was settled to: it is pending again, and its image loses
	// the key's reference. Its slot keeps the old view, which frames in flight may still sample;
	// a new resolution takes a new slot. True when the image has no reference left, so it need
	// not stay pinned in the texture cache.
	[[nodiscard]] bool ReleaseKey(Heap& heap, uint32_t key);

	// A guest sampler heap as one kind of use samples it: a region of the sampler array that
	// mirrors its S# records, key for key. Uses differ in whether the depth-compare function is
	// kept, whether point filtering is forced and whether border colours are integer, so each
	// kind gets its own region.
	static constexpr uint32_t SamplerDepthCompare   = 1u;
	static constexpr uint32_t SamplerPointFiltering = 2u;
	static constexpr uint32_t SamplerIntegerBorder  = 4u;
	struct SamplerHeap {
		uint64_t                             base          = 0;
		uint32_t                             table_offset  = 0;
		uint32_t                             record_stride = 16; // bytes between keys' S#s
		uint32_t                             flags         = 0;
		uint32_t                             region        = 0; // first slot; 0 = none yet
		uint32_t                             capacity      = 0;
		std::vector<std::array<uint32_t, 4>> records;           // mirrored S# records
	};
	[[nodiscard]] SamplerHeap* FindOrCreateSamplerHeap(uint64_t base, uint32_t table_offset,
	                                                   uint32_t record_stride, uint32_t flags);
	// Mirrors the heap's records into its region. New records are appended in place (their slots
	// were never used); a changed record, or more records than the region holds, moves the heap
	// to a new region, because slots the GPU may be reading are never rewritten. False when the
	// sampler array is full; the caller publishes the default sampler with a zero entry count.
	bool MirrorSamplerHeap(SamplerHeap& heap, std::span<const std::array<uint32_t, 4>> records,
	                       SamplerCache& cache);
	// Slot 0 of the sampler array, used for keys outside their heap; written once.
	void WriteDefaultSampler(vk::Sampler sampler);
	[[nodiscard]] bool SamplersEnabled() const noexcept { return m_samplers_per_array != 0; }
	// The texture cache dropped a resolved image: point its slots back at the placeholder and
	// make its keys pending again, so a draw that still needs it asks for it anew.
	void OnImageUnregistered(ImageId id);
    // Retirement callbacks only queue identities; producer applies them after draining users.
    void QueueUnregistered(ImageId id);
    void ApplyUnregistered();
    [[nodiscard]] uint32_t FindSlot(uint32_t binding, vk::ImageView view) const;
    // The view image slot `slot` of `binding` holds (null: a placeholder or never written).
    [[nodiscard]] vk::ImageView SlotView(uint32_t binding, uint32_t slot) const noexcept {
        if (binding >= ImageArrays || slot >= m_slot_views[binding].size()) return {};
        return m_slot_views[binding][slot];
    }
    void MarkUsed() { m_used = true; }
    [[nodiscard]] bool Used() const { return m_used; }
    void ClearUsed() { m_used = false; }

private:
	[[nodiscard]] bool AllocateRegion(Heap& heap, uint32_t entries);
	void EvictHeap(Heap& heap);
	// A region of `size` entries no command that may still run reads (recycled or new), or 0.
	[[nodiscard]] uint32_t TakeRegion(uint32_t size);
	// The region may be reused once every command recorded so far has completed.
	void RetireRegion(uint32_t start, uint32_t size);
	[[nodiscard]] bool RegionInUse(uint32_t start);
	// Every value of the heap into its region, and its feedback cleared.
	void WriteRegion(Heap& heap);
	// The slot no longer belongs to any view: FindSlot cannot return it, so it is never chosen
	// again. Its descriptor is left as it is: no translation names the slot once the keys that
	// did are published, and a partially bound descriptor no shader uses is not a reference.
	void ForgetSlot(uint32_t binding, uint32_t slot);
	// Every slot the image owns: forgotten, and reusable once the commands recorded so far
	// completed (one tick later, in case a submit lands before the commit of a prepared stage).
	void ReleaseImageSlots(ImageId id);
	// Slot 0 of every image array: a 1x1 grey texture of that view type, sampled for keys
	// outside a heap and while a texture is pending. Created, cleared and made read-only once.
	void CreatePlaceholders(CommandScheduler& scheduler);

	[[nodiscard]] static uint64_t ImageKey(ImageId id) {
		return static_cast<uint64_t>(id.index) | (static_cast<uint64_t>(id.generation) << 32u);
	}

	// Where a heap's descriptor table is: its base, the offset and stride of its records, and its
	// image array (or, for a sampler heap, its flags). FindOrCreateHeap and
	// FindOrCreateSamplerHeap look heaps up by it; a scan of every heap per bindless binding was
	// 26.5% of Thread_Gpu in Ghost of Yotei once its strided compute shaders ran.
	struct HeapPlace {
		uint64_t base          = 0;
		uint32_t table_offset  = 0;
		uint32_t record_stride = 0;
		uint32_t binding       = 0;
		bool     operator==(const HeapPlace&) const = default;
	};
	struct HeapPlaceHash {
		size_t operator()(const HeapPlace& place) const noexcept {
			uint64_t hash = place.base * 0x9e3779b97f4a7c15ull;
			hash ^= (static_cast<uint64_t>(place.table_offset) << 32u | place.record_stride) +
			        0x632be59bd9b4e019ull + (hash << 6u) + (hash >> 2u);
			hash ^= place.binding + 0x94d049bb133111ebull + (hash << 6u) + (hash >> 2u);
			return static_cast<size_t>(hash ^ (hash >> 29u));
		}
	};

	GraphicContext&         m_graphics;
	CommandScheduler&       m_scheduler;
	std::mutex m_retirement_mutex;
	std::vector<ImageId> m_unregistered;
	bool m_used = false;
	std::deque<Heap>        m_heaps;
	// The heaps at each place (interpretations of one table differ by ImageResource; usually one).
	std::unordered_map<HeapPlace, std::vector<Heap*>, HeapPlaceHash> m_heap_index;
	std::vector<Heap*> m_free_heaps;        // evicted heap objects, reused by FindOrCreateHeap
	uint32_t           m_heaps_since_sweep = 0;
	std::unordered_map<uint64_t, std::vector<std::pair<Heap*, uint32_t>>> m_image_refs;
	std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> m_image_slots;
	uint32_t                m_next_region = 1; // translation[0] is the out-of-range entry
	struct FreeRegion {
		uint32_t start = 0;
		uint32_t size  = 0;
	};
	std::vector<FreeRegion> m_free_regions;
	// Per region start (live or free), the last tick whose commands may read it.
	std::unordered_map<uint32_t, uint64_t> m_region_ticks;
	PublishStats                           m_publish_stats;
	std::array<uint32_t, ImageArrays> m_next_slot {PlaceholderColors, PlaceholderColors,
	                                               PlaceholderColors, PlaceholderColors};
	struct FreeSlot {
		uint32_t slot = 0;
		uint64_t tick = 0; // reusable once this tick completed
	};
	std::array<std::deque<FreeSlot>, ImageArrays> m_free_slots;
	std::array<VulkanImage, Placeholders>   m_placeholders;
	std::array<vk::ImageView, Placeholders> m_placeholder_views {};
	vk::DescriptorPool      m_pool   = nullptr;
	vk::DescriptorSetLayout m_layout = nullptr;
	vk::DescriptorSet       m_set    = nullptr;
	uint32_t                m_images_per_array = 0;
	uint32_t                m_samplers_per_array = 0;
	uint32_t                m_next_sampler_slot  = 1; // slot 0 is the default sampler
	bool                    m_default_sampler_written = false;
	std::deque<SamplerHeap> m_sampler_heaps;
	std::unordered_map<HeapPlace, SamplerHeap*, HeapPlaceHash> m_sampler_heap_index; // binding: flags
	std::unique_ptr<Buffer> m_translation;
	std::unique_ptr<Buffer> m_feedback;
	// The view each image slot holds, and the slots each view is in (placeholders excluded).
	std::array<std::vector<vk::ImageView>, ImageArrays>                        m_slot_views;
	std::unordered_map<VkImageView, std::vector<std::pair<uint32_t, uint32_t>>> m_view_slots;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDLESSTABLE_H_
