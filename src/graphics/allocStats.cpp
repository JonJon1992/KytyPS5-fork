// KYTY_FPS_LOG: heap allocations per second, next to the frame rate ([alloc] lines, renderDoc.cpp).
// Every C++ allocation of the process goes through these replacements of the global operator new
// and delete; they count into per-thread slots (a relaxed add on a line no other thread writes)
// and use malloc and free as the default ones do.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace Libs::Graphics {

namespace {
struct alignas(64) AllocSlot {
	std::atomic<uint64_t> count {0};
};
constexpr uint32_t        AllocSlots = 64;
AllocSlot                 g_alloc_slots[AllocSlots];
std::atomic<uint32_t>     g_alloc_next_slot {0};

inline void CountAlloc() {
	thread_local AllocSlot* slot =
	    &g_alloc_slots[g_alloc_next_slot.fetch_add(1, std::memory_order_relaxed) % AllocSlots];
	slot->count.fetch_add(1, std::memory_order_relaxed);
}

void* Allocate(std::size_t size) {
	CountAlloc();
	for (;;) {
		if (void* p = std::malloc(size != 0 ? size : 1); p != nullptr) {
			return p;
		}
		if (auto handler = std::get_new_handler(); handler != nullptr) {
			handler();
		} else {
			std::abort(); // out of memory
		}
	}
}

void* AllocateAligned(std::size_t size, std::align_val_t alignment) {
	CountAlloc();
	const auto align = static_cast<std::size_t>(alignment);
	for (;;) {
#if defined(_WIN32)
		void* p = _aligned_malloc(size != 0 ? size : 1, align);
#else
		void* p = std::aligned_alloc(align, ((size != 0 ? size : 1) + align - 1) / align * align);
#endif
		if (p != nullptr) {
			return p;
		}
		if (auto handler = std::get_new_handler(); handler != nullptr) {
			handler();
		} else {
			std::abort(); // out of memory
		}
	}
}

void FreeAligned(void* p) {
#if defined(_WIN32)
	_aligned_free(p);
#else
	std::free(p);
#endif
}
} // namespace

uint64_t AllocCountTotal() {
	uint64_t total = 0;
	for (const auto& slot: g_alloc_slots) {
		total += slot.count.load(std::memory_order_relaxed);
	}
	return total;
}

} // namespace Libs::Graphics

using Libs::Graphics::Allocate;
using Libs::Graphics::AllocateAligned;
using Libs::Graphics::FreeAligned;

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return Allocate(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return Allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) { return AllocateAligned(size, alignment); }
void* operator new[](std::size_t size, std::align_val_t alignment) { return AllocateAligned(size, alignment); }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
	return AllocateAligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
	return AllocateAligned(size, alignment);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { FreeAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { FreeAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { FreeAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { FreeAligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { FreeAligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { FreeAligned(p); }
