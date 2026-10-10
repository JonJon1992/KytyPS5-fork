#include "graphics/host_gpu/renderer/pipeline/textureBindingMemo.h"

#include "common/assert.h"
#include "common/liveSwitch.h"
#include "common/profiler.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/drawPrep/bindingPlan.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <xxhash.h>

namespace Libs::Graphics {

struct TextureBindingMemo::Entry {
	Key key;
	// 0: empty. Unique per recording, so a binding that holds this tag holds this description.
	uint64_t tag = 0;
	// The ImagePageTable page holding the description's first byte, and its structure version
	// (TextureCache::PageVersion) when recorded (both unused for null images).
	uint64_t page         = 0;
	uint64_t page_version = 0;
	ImageId  image;
	// TextureCache::RequestedFirstLevel of the description for this image: FindImage and
	// FindTexture extend the image's resident levels (EnsureResidency) unless its resident_first
	// is at most this.
	uint32_t requested_first = 0;
	// Another registered image has the same backing range, extent and sample count, so
	// SyncAliasFromOwner may copy into this one unless it owns the bytes.
	bool                    has_partner = false;
	bool                    null_image  = false;
	// FindImage's exact_format for the description (KYTY_TEXTURE_MEMO_REVALIDATE).
	bool                    exact_format = false;
	vk::ImageView           view        = nullptr;
	TextureCache::ImageDesc desc;
	// KYTY_CP_COMMIT=texdcc: a DCC description, recorded with the certificate of its FindImage's
	// DCC decision (MaterializeDccClear's MetadataNoop); false for every other entry.
	bool                       dcc = false;
	TextureCache::MetadataNoop dcc_noop;
	// FindHint's view of `key` and `tag` (P4b-2): Record makes `seq` odd, stores the packed key
	// and the tag, and makes it even again (release); a reader that sees the same even value
	// before and after its loads read a pair Record wrote together.
	// The memo's use clock at the last hit or fill (two-choice replacement picks the older).
	uint64_t                                      last_use = 0;
	std::atomic<uint32_t>                         seq {0};
	std::atomic<uint64_t>                         published_tag {0};
	std::array<std::atomic<uint64_t>, KeyWords>   packed_key {};
};

namespace {

// KYTY_TEXTURE_MEMO_REVALIDATE (default on; =0 off): an entry whose first page changed its owner
// list (the page version moved: an image registered or unregistered there, often a transient
// render target sharing the 1 MiB page) is checked again instead of being dropped: the lookup
// FindImage makes (FindImageWithSameBacking on the recorded description), the alias-partner scan
// and the version are redone exactly as Record() does them, under the same lock. If the lookup
// still returns the recorded image, the entry (and its view) stay valid for the new version.
bool RevalidateEnabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_TEXTURE_MEMO_REVALIDATE");
		return value == nullptr || std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

// KYTY_TEXTURE_MEMO_REVALIDATE_VERIFY=1|exit: every revalidated hit is followed by the full
// resolution (RenderExecutor::ResolveTexture), and a different image or description is counted
// (TextureBindingMemoRevalidateMismatches; exit stops on the first).
int RevalidateVerifyMode() {
	static const int mode = [] {
		const auto* value = std::getenv("KYTY_TEXTURE_MEMO_REVALIDATE_VERIFY");
		if (value == nullptr || *value == '\0' || std::strcmp(value, "0") == 0) {
			return 0;
		}
		return std::strcmp(value, "exit") == 0 ? 2 : 1;
	}();
	return mode;
}

} // namespace

bool TextureBindingMemo::HasPartner(const TextureCache& cache, uint64_t page, ImageId found,
                                    const Image& image) {
	bool partner = false;
	if (const auto* owners = cache.m_image_page_table.Find(page); owners != nullptr) {
		owners->ForEach([&](ImageId id) {
			const auto* other = cache.m_slot_images.try_get(id);
			if (id != found && other != nullptr && other->registered &&
			    other->info.data == image.info.data && other->info.extent == image.info.extent &&
			    other->backing.samples == image.backing.samples) {
				partner = true;
			}
		});
	}
	return partner;
}

bool TextureBindingMemo::RevalidateVerify() {
	return RevalidateVerifyMode() != 0;
}

void TextureBindingMemo::ReportRevalidateMismatch() {
	Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoRevalidateMismatches);
	static std::atomic<uint32_t> logged {0};
	if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
		std::fprintf(stderr, "TextureMemoRevalidateVerify: a revalidated entry resolved differently\n");
	}
	if (RevalidateVerifyMode() == 2) {
		EXIT("TextureMemoRevalidateVerify: a revalidated entry resolved differently\n");
	}
}

uint32_t TextureBindingMemo::SelectCapacity(const char* setting, bool bindless) noexcept {
	// Bindless heaps cycle through tens of thousands of exact keys, which evict each other in the
	// ordinary-draw capacity. An invalid override keeps the default.
	const uint32_t fallback = bindless ? MaxSlots : DefaultSlots;
	if (setting == nullptr) {
		return fallback;
	}
	const std::string_view text(setting);
	uint32_t               slots = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), slots);
	if (error != std::errc {} || end != text.data() + text.size() || slots < MinSlots ||
	    slots > MaxSlots || !std::has_single_bit(slots)) {
		return fallback;
	}
	return slots;
}

// Selected once, before any entry is published to DrawPrep readers; lookups never parse.
TextureBindingMemo::TextureBindingMemo(bool bindless)
    : m_slot_mask(SelectCapacity(std::getenv("KYTY_TEXTURE_BINDING_MEMO_SLOTS"), bindless) - 1u) {}
TextureBindingMemo::~TextureBindingMemo() = default;

bool TextureBindingMemo::Enabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_TEXTURE_BINDING_MEMO");
		return value == nullptr || std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

TextureBindingMemo::Key TextureBindingMemo::MakeKey(const ShaderRecompiler::IR::ImageResource& resource,
                                                    const uint32_t (&words)[8]) {
	Key key;
	std::memcpy(key.words.data(), words, sizeof(words));
	key.resource_class    = resource.resource_class;
	key.numeric_class     = resource.numeric_class;
	key.dimension         = resource.dimension;
	key.mip_mode          = resource.mip_mode;
	key.mip_count         = resource.mip_count;
	key.conversion_format = resource.conversion_format;
	key.shader_swizzle    = resource.shader_swizzle;
	key.read              = resource.read;
	key.written           = resource.written;
	key.atomic            = resource.atomic;
	key.atomic64          = resource.atomic64;
	key.depth_compare     = resource.depth_compare;
	key.cube              = resource.cube;
	key.r128              = resource.r128;
	key.packed            = resource.packed;
	return key;
}

uint64_t TextureBindingMemo::Hash(const Key& key) {
	return XXH3_64bits_withSeed(key.words.data(), sizeof(key.words),
	                            (static_cast<uint64_t>(key.dimension) << 32u) ^
	                                (static_cast<uint64_t>(key.numeric_class) << 16u) ^
	                                (key.written ? 1u : 0u) ^ (key.depth_compare ? 2u : 0u) ^
	                                (key.packed ? 4u : 0u) ^
	                                (static_cast<uint64_t>(key.mip_mode) << 8u));
}

bool TextureBindingMemo::RefreshIsNoOp(const Image& image) {
	// Mirrors TextureCache::RefreshImage for a registered image without a stencil association:
	// TrackImage returns at once when the resident range (`live`) is watched as a whole, or, for
	// a chunk-tracked image, when it is watched and no chunk was released; the maybe-dirty edge
	// hash runs only for a maybe-dirty image; InitializeImage (upload, partial or whole) runs
	// only for a buffer-modified or CPU-dirty image (chunk writes and residency extensions mark
	// the image CPU-dirty). Any new refresh trigger added there must be added here.
	if (image.IsCpuDirty() || image.IsBufferModified() || !image.IsTracked()) {
		return false;
	}
	if (image.ChunkTracked()) {
		return image.chunks.untracked_count == 0;
	}
	return image.track_addr == image.live.address && image.track_addr_end == image.live.End();
}

void TextureBindingMemo::Forget(TextureBinding& binding) {
	binding.memo_tag  = 0;
	binding.memo_slot = 0;
}

bool TextureBindingMemo::DccStateHolds(TextureCache& cache, const Entry& entry) {
	// What MaterializeDccClear's decision read outside the texture-cache lock (the recorded fill,
	// the GPU-dirty state of the metadata, the guest key bytes), as TryRepeatLookup checks it.
	if (cache.MetadataStateHolds(entry.dcc_noop)) {
		return true;
	}
	Profiler::CountFrameEvent(Profiler::FrameEvent::CpCommitTexDccRejects);
	return false;
}

void TextureBindingMemo::ApplyDcc(TextureCache& cache, const Entry& entry, Image& image) {
	// MaterializeDccClear's bookkeeping before its decision: the image takes the description's
	// metadata, and no other interpretation of those bytes remains (TryRepeatLookup does the same).
	image.info.metadata = entry.desc.info.metadata;
	cache.EraseSurfaceMeta(entry.desc.info.metadata.range.address);
}

TextureBindingMemo::PackedKey TextureBindingMemo::PackKey(const Key& key) {
	PackedKey packed {};
	for (uint32_t i = 0; i < 4; i++) {
		packed[i] = key.words[2 * i] | (static_cast<uint64_t>(key.words[2 * i + 1]) << 32u);
	}
	packed[4] = key.mip_count | (static_cast<uint64_t>(key.shader_swizzle) << 32u);
	packed[5] = static_cast<uint32_t>(key.resource_class) |
	            (static_cast<uint64_t>(static_cast<uint32_t>(key.numeric_class)) << 32u);
	packed[6] = static_cast<uint32_t>(key.dimension) |
	            (static_cast<uint64_t>(static_cast<uint32_t>(key.mip_mode)) << 32u);
	const uint64_t flags = (key.read ? 1u : 0u) | (key.written ? 2u : 0u) | (key.atomic ? 4u : 0u) |
	                       (key.depth_compare ? 8u : 0u) | (key.cube ? 16u : 0u) |
	                       (key.r128 ? 32u : 0u) | (key.atomic64 ? 64u : 0u) |
	                       (key.packed ? 128u : 0u);
	packed[7] = static_cast<uint32_t>(key.conversion_format) | (flags << 32u);
	return packed;
}

// KYTY_TEXTURE_MEMO_TWO_CHOICE (default on; =0 off, live): Record places a key in the better of
// its two slots (SlotsOf) instead of always the first. Lookups probe both either way, so the
// switch only changes where new entries go.
static Live::Switch g_memo_two_choice("KYTY_TEXTURE_MEMO_TWO_CHOICE", Live::ParseDefaultOn);

std::array<uint32_t, 2> TextureBindingMemo::SlotsOf(uint64_t hash) const noexcept {
	const auto first  = static_cast<uint32_t>(hash & m_slot_mask);
	// Bits of the hash the first slot does not use, mixed so that equal first slots spread.
	auto       second = static_cast<uint32_t>(((hash >> 32u) * 0x9e3779b1u) & m_slot_mask);
	if (second == first) {
		second ^= 1u;
	}
	return {first, second};
}

uint32_t TextureBindingMemo::SlotWithTag(uint64_t hash, uint64_t tag) const noexcept {
	if (!m_entries || tag == 0) {
		return UINT32_MAX;
	}
	for (const auto slot: SlotsOf(hash)) {
		if (m_entries[slot].tag == tag) {
			return slot;
		}
	}
	return UINT32_MAX;
}

bool TextureBindingMemo::FindHint(const Key& key, uint64_t hash, uint64_t& tag) const {
	const auto* entries = m_published.load(std::memory_order_acquire);
	if (entries == nullptr) {
		return false;
	}
	const auto packed = PackKey(key);
	for (const auto slot: SlotsOf(hash)) {
		const auto& entry  = entries[slot];
		const auto  before = entry.seq.load(std::memory_order_acquire);
		if ((before & 1u) != 0) {
			continue; // being rewritten
		}
		const auto found = entry.published_tag.load(std::memory_order_relaxed);
		bool       same  = found != 0;
		for (uint32_t i = 0; i < KeyWords; i++) {
			same = entry.packed_key[i].load(std::memory_order_relaxed) == packed[i] && same;
		}
		std::atomic_thread_fence(std::memory_order_acquire);
		if (!same || entry.seq.load(std::memory_order_relaxed) != before) {
			continue;
		}
		tag = found;
		return true;
	}
	return false;
}

Image* TextureBindingMemo::HitImage(TextureCache& cache, const Entry& entry) {
	auto* image = cache.m_slot_images.try_get(entry.image);
	if (image == nullptr || !image->registered || image->depth_id ||
	    image->binding.needs_rebind || entry.requested_first < image->resident_first ||
	    cache.PageVersion(entry.page) != entry.page_version ||
	    (entry.has_partner && !image->alias_owner && !image->info.HasStencil())) {
		return nullptr;
	}
	return image;
}

void TextureBindingMemo::ApplyHit(Entry& entry, uint32_t slot, TextureBinding& binding) {
	binding.image_id = entry.image;
	entry.last_use   = ++m_use_clock;
	if (binding.memo_tag == entry.tag && binding.memo_slot == slot) {
		Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingDescCopiesAvoided);
	} else {
		binding.desc      = entry.desc;
		binding.memo_tag  = entry.tag;
		binding.memo_slot = slot;
	}
	Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoHits);
	m_totals.hits++;
}

uint32_t TextureBindingMemo::TryResolveRun(TextureCache& cache, std::span<const uint64_t> hashes,
                                           std::span<const uint64_t> tags,
                                           std::span<TextureBinding> bindings, bool apply) {
	m_last_revalidated = false;
	m_last_dcc_hit     = false;
	EXIT_IF(hashes.size() < bindings.size() || tags.size() < bindings.size());
	if (!m_entries || bindings.empty() || tags[0] == 0) {
		return 0;
	}
	uint32_t         hits = 0;
	std::scoped_lock lock {cache.m_lock};
	const auto       tick = cache.m_scheduler.CurrentTick();
	for (; hits < bindings.size(); hits++) {
		const auto slot = SlotWithTag(hashes[hits], tags[hits]);
		if (slot == UINT32_MAX) {
			break;
		}
		auto& entry = m_entries[slot];
		if (entry.dcc) {
			break; // TryResolve checks the DCC certificate outside the lock
		}
		Image* image = nullptr;
		if (!entry.null_image) {
			image = HitImage(cache, entry);
			if (image == nullptr) {
				break; // TryResolve decides: stale, or a revalidation
			}
		}
		if (!apply) {
			continue;
		}
		if (image != nullptr) {
			// FindImage's access bookkeeping for the returned image (as TryResolve).
			image->tick_accessed_last = tick;
			cache.TouchImage(*image);
		}
		ApplyHit(entry, slot, bindings[hits]);
	}
	return hits;
}

bool TextureBindingMemo::TryResolve(TextureCache& cache, const Key& key, uint64_t hash,
                                    TextureBinding& binding, uint64_t tag_hint, bool verify_hint) {
	m_last_revalidated = false;
	m_last_dcc_hit     = false;
	if (!m_entries) {
		Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoMisses);
		return false;
	}
	// The slot holding the key: the one whose tag a hint names (FindHint proved the key), else
	// the one whose key compares equal.
	uint32_t   slot   = SlotWithTag(hash, tag_hint);
	const bool hinted = slot != UINT32_MAX;
	if (!hinted) {
		slot = UINT32_MAX;
		for (const auto candidate: SlotsOf(hash)) {
			if (m_entries[candidate].tag != 0 && m_entries[candidate].key == key) {
				slot = candidate;
				break;
			}
		}
		if (slot == UINT32_MAX) {
			Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoMisses);
			return false;
		}
	}
	auto& entry = m_entries[slot];
	if (hinted && verify_hint) {
		DrawPrep::CountBindingVerifyCheck();
		if (!(entry.key == key)) {
			DrawPrep::ReportBindingMismatch("texture memo hint");
		}
	}
	if (entry.tag == 0 || !(hinted || entry.key == key)) {
		Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoMisses);
		return false;
	}
	// KYTY_CP_COMMIT=texdcc: MaterializeDccClear would decide the same, and do nothing but its
	// bookkeeping (below), only while the recorded certificate holds.
	if (entry.dcc && !DccStateHolds(cache, entry)) {
		Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoStale);
		m_totals.stale++;
		return false;
	}
	bool revalidated = false;
	if (!entry.null_image) {
		std::scoped_lock lock {cache.m_lock};
		auto*            image = cache.m_slot_images.try_get(entry.image);
		const auto       stale = [&] {
            Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoStale);
            m_totals.stale++;
            return false;
		};
		if (entry.dcc && !cache.MetadataPagesHold(entry.dcc_noop)) {
			Profiler::CountFrameEvent(Profiler::FrameEvent::CpCommitTexDccRejects);
			return stale();
		}
		// A stencil association redirects the binding; a pending rebind, a copy from the alias
		// owner and a residency extension (resident levels changed, or fewer than the view
		// samples) are FindImage/RebindImages work: all take the full resolution.
		if (image == nullptr || !image->registered || image->depth_id ||
		    image->binding.needs_rebind || entry.requested_first < image->resident_first) {
			return stale();
		}
		// A structural change on the description's first page may alter what FindImage returns:
		// redo its lookup and the partner scan for the new owner list (RevalidateEnabled).
		if (const auto version = cache.PageVersion(entry.page); entry.page_version != version) {
			if (!RevalidateEnabled() ||
			    cache.FindImageWithSameBacking(entry.desc.info, entry.exact_format) !=
			        entry.image) {
				return stale();
			}
			entry.has_partner  = HasPartner(cache, entry.page, entry.image, *image);
			entry.page_version = version;
			revalidated        = true;
			Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoRevalidated);
			m_totals.revalidated++;
		}
		if (entry.has_partner && !image->alias_owner && !image->info.HasStencil()) {
			return stale();
		}
		// FindImage's access bookkeeping for the returned image.
		image->tick_accessed_last = cache.m_scheduler.CurrentTick();
		cache.TouchImage(*image);
		if (entry.dcc) {
			ApplyDcc(cache, entry, *image);
		}
	}
	ApplyHit(entry, slot, binding);
	m_last_revalidated = revalidated;
	m_last_dcc_hit     = entry.dcc;
	return true;
}

void TextureBindingMemo::Record(TextureCache& cache, const Key& key, uint64_t hash,
                                TextureBinding& binding, ImageId found, bool exact_format,
                                bool view_rebased, const TextureCache::MetadataNoop* dcc) {
	Forget(binding);
	const auto& desc = binding.desc;
	// A DCC description only with the certificate of its lookup's DCC decision (texdcc).
	const bool dcc_desc = desc.info.metadata.kind == ImageMetadataKind::Dcc;
	if (view_rebased || binding.image_id != found ||
	    (dcc_desc && (dcc == nullptr || !dcc->provable))) {
		Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoRejects);
		return;
	}
	const bool null_image      = desc.info.data.Empty();
	bool       has_partner     = false;
	uint64_t   page            = 0;
	uint64_t   page_version    = 0;
	uint32_t   requested_first = 0;
	{
		// One critical section: the checks below and the page version describe the same state.
		std::scoped_lock lock {cache.m_lock};
		const auto*      image = cache.m_slot_images.try_get(found);
		if (null_image) {
			// FindImage returned the null image of this format; it is never registered or freed.
			if (image == nullptr || image->registered || !image->info.data.Empty()) {
				Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoRejects);
				return;
			}
		} else {
			// Only first-page answers: FindImage would return `found` again, and nothing else,
			// while the owner list of the description's first page is unchanged (SameBacking
			// requires the same start address, so `found` starts on that page too).
			TextureCache::ImagePageTable::PageRange pages {};
			if (cache.m_image_lookup_mode != TextureCache::ImageLookupMode::FirstPage ||
			    image == nullptr || !image->registered || image->depth_id ||
			    !TextureCache::ImagePageTable::TryGetPageRange(image->info.data.address,
			                                                   image->info.data.size, pages) ||
			    cache.FindImageWithSameBacking(desc.info, exact_format) != found) {
				Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoRejects);
				return;
			}
			// What FindImage/FindTexture pass to EnsureResidency for this description; they
			// already made these levels resident, and only EnsureResidency (re-registration)
			// changes resident_first.
			requested_first = cache.RequestedFirstLevel(desc, image->info.resources.levels);
			// SyncAliasFromOwner copies only from images with this exact backing range, extent
			// and sample count; they start on the same indexed page as this one. Their flags
			// (alias owner, GPU-modified, stencil) are not considered: any such image makes the
			// entry usable only while this image owns the bytes.
			has_partner  = HasPartner(cache, pages.first, found, *image);
			page         = pages.first;
			page_version = cache.PageVersion(page);
		}
	}
	if (!m_entries) {
		m_entries = std::make_unique<Entry[]>(m_slot_mask + 1u);
		m_published.store(m_entries.get(), std::memory_order_release);
	}
	// The slot of the same key if it holds one; else (two-choice placement) an empty slot, else
	// the one hit least recently; the first slot when the switch is off (direct-mapped).
	const auto candidates = SlotsOf(hash);
	uint32_t   slot       = candidates[0];
	if (g_memo_two_choice.On()) {
		const auto& a = m_entries[candidates[0]];
		const auto& b = m_entries[candidates[1]];
		if (a.tag != 0 && a.key == key) {
			slot = candidates[0];
		} else if (b.tag != 0 && b.key == key) {
			slot = candidates[1];
		} else if (a.tag == 0) {
			slot = candidates[0];
		} else if (b.tag == 0) {
			slot = candidates[1];
		} else {
			slot = a.last_use <= b.last_use ? candidates[0] : candidates[1];
		}
	} else if (const auto& b = m_entries[candidates[1]]; b.tag != 0 && b.key == key) {
		slot = candidates[1]; // keep a single entry per key
	}
	auto& entry = m_entries[slot];
	entry.last_use = ++m_use_clock;
	// FindHint's readers: odd while the key and tag change (the fence orders the odd value before
	// the new words), even again once both are stored (release).
	const auto seq = entry.seq.load(std::memory_order_relaxed);
	entry.seq.store(seq + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	entry.key             = key;
	entry.tag             = m_next_tag++;
	const auto packed     = PackKey(key);
	for (uint32_t i = 0; i < KeyWords; i++) {
		entry.packed_key[i].store(packed[i], std::memory_order_relaxed);
	}
	entry.published_tag.store(entry.tag, std::memory_order_relaxed);
	entry.seq.store(seq + 2, std::memory_order_release);
	entry.page            = page;
	entry.page_version    = page_version;
	entry.image           = found;
	entry.requested_first = requested_first;
	entry.has_partner     = has_partner;
	entry.null_image      = null_image;
	entry.exact_format    = exact_format;
	entry.view            = nullptr;
	entry.desc            = desc;
	entry.dcc             = dcc_desc;
	entry.dcc_noop        = dcc_desc ? *dcc : TextureCache::MetadataNoop {};
	binding.memo_tag      = entry.tag;
	binding.memo_slot     = slot;
	Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoFills);
	if (dcc_desc) {
		Profiler::CountFrameEvent(Profiler::FrameEvent::CpCommitTexDccRecords);
	}
}

const TextureBindingMemo::Entry* TextureBindingMemo::ViewEntry(const TextureBinding& binding) const {
	if (!m_entries || binding.memo_tag == 0 ||
	    binding.desc.type != TextureCache::BindingType::Texture) {
		return nullptr;
	}
	const auto& entry = m_entries[binding.memo_slot & m_slot_mask];
	if (entry.tag != binding.memo_tag || entry.image != binding.image_id || entry.view == nullptr) {
		return nullptr;
	}
	return &entry;
}

bool TextureBindingMemo::ViewImageReady(const Entry& entry, const Image& image) {
	// FindTexture's rediscovery checks, a no-op EnsureResidency (the view's levels are resident),
	// a no-op RefreshImage, and no stencil plane refresh.
	return image.info.data.Empty() ||
	       (image.registered && !image.depth_id && !image.binding.needs_rebind &&
	        entry.requested_first >= image.resident_first && !image.info.HasStencil() &&
	        RefreshIsNoOp(image));
}

bool TextureBindingMemo::TryAcquireView(TextureCache& cache, TextureBinding& binding) {
	if (!m_entries || binding.memo_tag == 0 ||
	    binding.desc.type != TextureCache::BindingType::Texture) {
		return false;
	}
	const auto* entry = ViewEntry(binding);
	if (entry == nullptr) {
		Profiler::CountFrameEvent(Profiler::FrameEvent::TextureViewMemoMisses);
		return false;
	}
	std::scoped_lock lock {cache.m_lock};
	auto*            image = cache.m_slot_images.try_get(binding.image_id);
	if (image == nullptr || !ViewImageReady(*entry, *image)) {
		Profiler::CountFrameEvent(Profiler::FrameEvent::TextureViewMemoMisses);
		return false;
	}
	cache.TouchImage(*image);
	binding.image_view = entry->view;
	Profiler::CountFrameEvent(Profiler::FrameEvent::TextureViewMemoHits);
	return true;
}

uint32_t TextureBindingMemo::TryAcquireViewRun(TextureCache& cache,
                                               std::span<TextureBinding> bindings, bool apply) {
	if (!m_entries || bindings.empty() || ViewEntry(bindings[0]) == nullptr) {
		return 0;
	}
	uint32_t         hits = 0;
	std::scoped_lock lock {cache.m_lock};
	for (; hits < bindings.size(); hits++) {
		auto&       binding = bindings[hits];
		const auto* entry   = ViewEntry(binding);
		auto*       image   = entry != nullptr ? cache.m_slot_images.try_get(binding.image_id) : nullptr;
		if (image == nullptr || !ViewImageReady(*entry, *image)) {
			break; // TryAcquireView decides
		}
		if (apply) {
			cache.TouchImage(*image);
			binding.image_view = entry->view;
			Profiler::CountFrameEvent(Profiler::FrameEvent::TextureViewMemoHits);
		}
	}
	return hits;
}

bool TextureBindingMemo::TryRepeatResolve(TextureCache& cache, std::span<TextureBinding> bindings,
                                          bool apply) {
	m_last_revalidated = false;
	m_last_dcc_hit     = false;
	if (!m_entries) {
		return false;
	}
	// KYTY_CP_COMMIT=texdcc: DCC certificates first, outside the lock (as TryResolve checks them).
	for (const auto& binding: bindings) {
		const auto& entry = m_entries[binding.memo_slot & m_slot_mask];
		if (binding.memo_tag != 0 && entry.tag == binding.memo_tag && entry.dcc &&
		    !DccStateHolds(cache, entry)) {
			return false;
		}
	}
	std::scoped_lock lock {cache.m_lock};
	for (const auto& binding: bindings) {
		const auto& entry = m_entries[binding.memo_slot & m_slot_mask];
		if (binding.memo_tag == 0 || entry.tag != binding.memo_tag || entry.image != binding.image_id) {
			return false;
		}
		if (entry.null_image) {
			continue;
		}
		// TryResolve's conditions for a hit that needs no revalidation.
		const auto* image = cache.m_slot_images.try_get(entry.image);
		if (image == nullptr || !image->registered || image->depth_id ||
		    image->binding.needs_rebind || entry.requested_first < image->resident_first ||
		    cache.PageVersion(entry.page) != entry.page_version ||
		    (entry.has_partner && !image->alias_owner && !image->info.HasStencil()) ||
		    (entry.dcc && !cache.MetadataPagesHold(entry.dcc_noop))) {
			return false;
		}
	}
	if (!apply) {
		return true;
	}
	const auto tick = cache.m_scheduler.CurrentTick();
	for (const auto& binding: bindings) {
		const auto& entry = m_entries[binding.memo_slot & m_slot_mask];
		if (!entry.null_image) {
			auto& image              = cache.m_slot_images[entry.image];
			image.tick_accessed_last = tick;
			cache.TouchImage(image);
			if (entry.dcc) {
				ApplyDcc(cache, entry, image);
			}
		}
	}
	// The hits TryResolve would count (each finds its binding's own entry: no description copy).
	Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingDescCopiesAvoided, bindings.size());
	Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoHits, bindings.size());
	m_totals.hits += bindings.size();
	return true;
}

bool TextureBindingMemo::TryRepeatViews(TextureCache& cache, std::span<TextureBinding> bindings,
                                        bool apply) {
	if (!m_entries) {
		return false;
	}
	std::scoped_lock lock {cache.m_lock};
	for (const auto& binding: bindings) {
		const auto& entry = m_entries[binding.memo_slot & m_slot_mask];
		if (binding.memo_tag == 0 || binding.desc.type != TextureCache::BindingType::Texture ||
		    entry.tag != binding.memo_tag || entry.image != binding.image_id ||
		    entry.view == nullptr) {
			return false;
		}
		const auto* image = cache.m_slot_images.try_get(binding.image_id);
		// RebindImages resolves a binding again when its image is gone, unregistered (null images
		// never are) or awaits a rebind; then TryAcquireView's conditions.
		if (image == nullptr || image->binding.needs_rebind) {
			return false;
		}
		if (!image->info.data.Empty() &&
		    (!image->registered || image->depth_id ||
		     entry.requested_first < image->resident_first || image->info.HasStencil() ||
		     !RefreshIsNoOp(*image))) {
			return false;
		}
	}
	if (!apply) {
		return true;
	}
	for (auto& binding: bindings) {
		const auto& entry = m_entries[binding.memo_slot & m_slot_mask];
		cache.TouchImage(cache.m_slot_images[binding.image_id]);
		binding.image_view = entry.view;
	}
	Profiler::CountFrameEvent(Profiler::FrameEvent::TextureViewMemoHits, bindings.size());
	return true;
}

bool TextureBindingMemo::TryRepeatKeys(TextureCache& cache, std::span<const RepeatKey> keys) {
	if (keys.empty()) {
		return true;
	}
	if (!m_entries) {
		return false;
	}
	std::scoped_lock lock {cache.m_lock};
	for (const auto& key: keys) {
		const auto slot = SlotWithTag(key.hash, key.tag);
		if (slot == UINT32_MAX) {
			return false;
		}
		const auto& entry = m_entries[slot];
		// TryResolve checks a DCC entry's certificate outside the lock: left to it.
		if (key.tag == 0 || entry.tag != key.tag || entry.image != key.image || entry.dcc ||
		    entry.null_image || entry.view == nullptr || entry.view != key.view) {
			return false;
		}
		const auto* image = HitImage(cache, entry);
		if (image == nullptr || !ViewImageReady(entry, *image)) {
			return false;
		}
	}
	const auto tick = cache.m_scheduler.CurrentTick();
	for (const auto& key: keys) {
		auto& image              = cache.m_slot_images[key.image];
		image.tick_accessed_last = tick;
		cache.TouchImage(image);
	}
	m_totals.hits += keys.size();
	Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoHits, keys.size());
	return true;
}

size_t TextureBindingMemo::TryRepeatEachKey(TextureCache& cache, std::span<const RepeatKey> keys,
                                            std::span<uint8_t> repeated) {
	EXIT_IF(repeated.size() != keys.size());
	std::ranges::fill(repeated, uint8_t {0});
	if (keys.empty() || !m_entries) {
		return 0;
	}
	size_t           hits = 0;
	std::scoped_lock lock {cache.m_lock};
	const auto       tick = cache.m_scheduler.CurrentTick();
	for (size_t i = 0; i < keys.size(); i++) {
		const auto& key  = keys[i];
		const auto  slot = SlotWithTag(key.hash, key.tag);
		if (slot == UINT32_MAX) {
			continue;
		}
		const auto& entry = m_entries[slot];
		// The checks of TryRepeatKeys (a DCC entry's certificate is left to TryResolve).
		if (key.tag == 0 || entry.tag != key.tag || entry.image != key.image || entry.dcc ||
		    entry.null_image || entry.view == nullptr || entry.view != key.view) {
			continue;
		}
		const auto* image = HitImage(cache, entry);
		if (image == nullptr || !ViewImageReady(entry, *image)) {
			continue;
		}
		auto& hit              = cache.m_slot_images[key.image];
		hit.tick_accessed_last = tick;
		cache.TouchImage(hit);
		repeated[i] = 1u;
		hits++;
	}
	m_totals.hits += hits;
	Profiler::CountFrameEvent(Profiler::FrameEvent::TextureBindingMemoHits, hits);
	return hits;
}

vk::ImageView TextureBindingMemo::EntryView(const TextureBinding& binding) const {
	if (!m_entries || binding.memo_tag == 0) {
		return nullptr;
	}
	const auto& entry = m_entries[binding.memo_slot & m_slot_mask];
	return entry.tag == binding.memo_tag ? entry.view : nullptr;
}

void TextureBindingMemo::RecordView(const TextureBinding& binding, vk::ImageView view) {
	if (!m_entries || binding.memo_tag == 0 || view == nullptr ||
	    binding.desc.type != TextureCache::BindingType::Texture) {
		return;
	}
	auto& entry = m_entries[binding.memo_slot & m_slot_mask];
	if (entry.tag == binding.memo_tag && entry.image == binding.image_id) {
		entry.view = view;
	}
}

} // namespace Libs::Graphics
