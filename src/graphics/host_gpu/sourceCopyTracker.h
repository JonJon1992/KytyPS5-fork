#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_SOURCECOPYTRACKER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_SOURCECOPYTRACKER_H_

#include "common/assert.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace Libs::Graphics {

// Sources retained by one FIFO host copier. A later ticket covers earlier overlapping
// copies because completion is contiguous. The worker only publishes to completed;
// recording and emulator writes use the interval map, never hold its lock while waiting.
class SourceCopyTracker final {
public:
	explicit SourceCopyTracker(std::atomic<uint64_t>& completed): m_completed(completed) {}

	void Track(uint64_t address, uint64_t size, uint64_t value) {
		const Source range {address, size};
		Track(std::span<const Source>(&range, 1), value);
	}

	// Register one job under one lock, without constructing another range vector.
	template <class Range>
	void Track(std::span<const Range> ranges, uint64_t value) {
		Track(ranges, value, [](const Range& range) { return range.guest_address; });
	}

	// Canonical backing addresses identify shared guest views; projection avoids another vector.
	template <class Range, class Address>
	void Track(std::span<const Range> ranges, uint64_t value, Address address) {
		if (ranges.empty()) {
			return;
		}
		std::scoped_lock lock(m_mutex);
		const auto latest = m_latest.load(std::memory_order_relaxed);
		EXIT_IF(value == 0 || value < latest);
		PruneLocked(m_completed.load(std::memory_order_acquire));
		for (const auto& range: ranges) {
			if (range.size == 0) {
				continue;
			}
			const auto begin = address(range);
			const auto end   = UINT64_MAX - begin < range.size ? UINT64_MAX : begin + range.size;
			// A job's runs are mostly contiguous: one entry per run of the same producer.
			if (m_head < m_sources.size() && m_sources.back().value == value &&
			    m_sources.back().end == begin) {
				m_sources.back().end = end;
			} else {
				m_sources.push_back({begin, end, value});
			}
		}
		m_latest.store(value, std::memory_order_release);
	}

	[[nodiscard]] bool IsIdle() const noexcept {
		return m_completed.load(std::memory_order_acquire) >= m_latest.load(std::memory_order_acquire);
	}

	[[nodiscard]] uint64_t PendingValue(uint64_t address, uint64_t size) const {
		if (size == 0 || IsIdle()) {
			return 0;
		}
		const auto end = UINT64_MAX - address < size ? UINT64_MAX : address + size;
		std::scoped_lock lock(m_mutex);
		const auto completed = m_completed.load(std::memory_order_acquire);
		// Values never decrease in insertion order: the newest overlapping source is the maximum,
		// and the scan ends at the first completed one (every older source is complete too).
		for (auto index = m_sources.size(); index > m_head; --index) {
			const auto& source = m_sources[index - 1];
			if (source.value <= completed) {
				break;
			}
			if (source.begin < end && address < source.end) {
				return source.value;
			}
		}
		return 0;
	}

	// PendingValue of the guest range [address, address + size), keyed as the sources are: by
	// canonical backing alias, which every guest view of the same direct memory shares. A range
	// spanning mappings (host-page aligned) is queried per run of adjacent aliases.
	[[nodiscard]] uint64_t PendingGuestValue(uint64_t address, uint64_t size) const {
		if (size == 0 || IsIdle()) {
			return 0;
		}
		EXIT_IF(UINT64_MAX - address < size);
		if (const auto* source = LibKernel::Memory::GuestBackingAlias(address, size); source != nullptr) {
			return PendingValue(reinterpret_cast<uint64_t>(source), size);
		}
		constexpr uint64_t Page = 4096;
		uint64_t value = 0, run = 0, run_size = 0;
		for (uint64_t done = 0; done < size;) {
			const auto current = address + done;
			const auto bytes   = std::min(size - done, Page - (current & (Page - 1)));
			const auto alias =
			    reinterpret_cast<uint64_t>(LibKernel::Memory::GuestBackingAlias(current, bytes));
			if (run_size != 0 && (alias == 0 || alias != run + run_size)) {
				value    = std::max(value, PendingValue(run, run_size));
				run_size = 0;
			}
			if (alias != 0) {
				if (run_size == 0) {
					run = alias;
				}
				run_size += bytes;
			}
			done += bytes;
		}
		return run_size == 0 ? value : std::max(value, PendingValue(run, run_size));
	}

	// Returns the relevant ticket, or zero if this range was already available.
	uint64_t WaitRange(uint64_t address, uint64_t size) const {
		const auto value = PendingValue(address, size);
		auto completed = m_completed.load(std::memory_order_acquire);
		while (completed < value) {
			m_completed.wait(completed, std::memory_order_acquire);
			completed = m_completed.load(std::memory_order_acquire);
		}
		return value;
	}

private:
	struct Source {
		uint64_t guest_address;
		uint64_t size;
	};
	// A tracked source interval and its producer value. Kept in insertion order, which is
	// non-decreasing value order (Track refuses a smaller value): pruning drops a prefix and a
	// query stops at the first completed entry, so neither allocates nor rebalances a tree.
	struct Entry {
		uint64_t begin;
		uint64_t end;
		uint64_t value;
	};

	// Drops the completed prefix; compacts once it is at least half the storage (amortized O(1),
	// the capacity is kept). m_mutex held.
	void PruneLocked(uint64_t completed) {
		while (m_head < m_sources.size() && m_sources[m_head].value <= completed) {
			m_head++;
		}
		if (m_head == m_sources.size()) {
			m_sources.clear();
			m_head = 0;
		} else if (m_head >= 64 && m_head * 2 >= m_sources.size()) {
			m_sources.erase(m_sources.begin(), m_sources.begin() + static_cast<std::ptrdiff_t>(m_head));
			m_head = 0;
		}
	}

	std::atomic<uint64_t>& m_completed;
	std::atomic<uint64_t> m_latest {0};
	mutable std::mutex m_mutex;
	std::vector<Entry> m_sources; // protected by m_mutex
	size_t m_head = 0;            // first entry not known complete; protected by m_mutex
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_SOURCECOPYTRACKER_H_
