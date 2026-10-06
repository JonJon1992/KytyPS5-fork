#pragma once

#include <array>
#include <cstddef>
#include <new>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Storage for instruction nodes and their argument/use lists. A translated program allocates
// three small blocks per instruction and frees them all together; through malloc that was half
// of translation time, plus page faults each time freeing a program returned the heap top to
// the system. Freed blocks stay in a per-thread cache by size class instead (capped), so the
// next program reuses them. Blocks are plain operator new memory: a block freed on another
// thread joins that thread's cache, or goes back to the heap.
namespace IrStorage {

inline constexpr size_t Granule      = 16;
inline constexpr size_t MaxBlockSize = 512;
inline constexpr size_t MaxCached    = size_t {16} << 20u;

struct FreeBlock {
	FreeBlock* next;
};

// Trivially destructible: it stays usable while other thread_local objects are destroyed.
struct Cache {
	std::array<FreeBlock*, MaxBlockSize / Granule> heads {};
	size_t                                         bytes   = 0;
	bool                                           drained = false;
};

inline thread_local constinit Cache cache;

struct Drain {
	Drain() = default;
	~Drain() {
		for (auto*& head: cache.heads) {
			while (head != nullptr) {
				auto* next = head->next;
				::operator delete(head);
				head = next;
			}
		}
		cache.bytes   = 0;
		cache.drained = true;
	}
};

inline thread_local Drain drain;

inline void* Allocate(size_t bytes) {
	if (bytes == 0 || bytes > MaxBlockSize) {
		return ::operator new(bytes);
	}
	const size_t slot = (bytes - 1u) / Granule;
	if (auto* block = cache.heads[slot]; block != nullptr) {
		cache.heads[slot] = block->next;
		cache.bytes -= (slot + 1u) * Granule;
		return block;
	}
	return ::operator new((slot + 1u) * Granule);
}

inline void Free(void* pointer, size_t bytes) {
	if (bytes == 0 || bytes > MaxBlockSize) {
		::operator delete(pointer);
		return;
	}
	const size_t slot = (bytes - 1u) / Granule;
	const size_t size = (slot + 1u) * Granule;
	if (cache.drained || cache.bytes + size > MaxCached) {
		::operator delete(pointer);
		return;
	}
	if (cache.bytes == 0) {
		// Registers the thread's drain on its first cached block.
		static_cast<void>(&drain);
	}
	auto* block       = static_cast<FreeBlock*>(pointer);
	block->next       = cache.heads[slot];
	cache.heads[slot] = block;
	cache.bytes += size;
}

} // namespace IrStorage

template <typename T>
struct IrAllocator {
	using value_type = T;

	IrAllocator() = default;
	template <typename U>
	IrAllocator(const IrAllocator<U>& /*other*/) noexcept {}

	[[nodiscard]] T* allocate(size_t count) {
		static_assert(alignof(T) <= IrStorage::Granule);
		return static_cast<T*>(IrStorage::Allocate(count * sizeof(T)));
	}
	void deallocate(T* pointer, size_t count) noexcept {
		IrStorage::Free(pointer, count * sizeof(T));
	}

	template <typename U>
	bool operator==(const IrAllocator<U>& /*other*/) const noexcept {
		return true;
	}
};

} // namespace Libs::Graphics::ShaderRecompiler::IR
