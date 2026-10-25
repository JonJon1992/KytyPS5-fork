#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_COVERAGE_SPANS_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_COVERAGE_SPANS_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Libs::LibKernel::Memory {

// A maximal run of committed guest memory [start, end), as TryClampRangeSize answers from.
struct CoverageSpan {
	uint64_t start = 0;
	uint64_t end   = 0;

	bool operator==(const CoverageSpan&) const = default;
};

// The span of the sorted, disjoint spans holding vaddr; the empty [vaddr, vaddr) when none does.
[[nodiscard]] inline CoverageSpan FindCoverageSpan(const std::vector<CoverageSpan>& spans,
                                                   uint64_t                         vaddr) {
	auto span = std::upper_bound(spans.begin(), spans.end(), vaddr,
	                             [](uint64_t value, const CoverageSpan& candidate) {
		                             return value < candidate.start;
	                             });
	if (span == spans.begin()) {
		return {vaddr, vaddr};
	}
	--span;
	return vaddr < span->end ? *span : CoverageSpan {vaddr, vaddr};
}

// The coverage changes since the spans were last built. Every change rewrites only the bytes of its
// window, so a span no window touches (ends included: a change next to a span can join or split
// it) is still exact, and so is "not covered" at an address no window touches. Lookups there need
// no rebuild of the spans (KYTY_CLAMP_SPANS_LOCAL, memory.cpp). Past Capacity windows every lookup
// counts as touched.
class CoverageChanges {
public:
	static constexpr size_t Capacity = 64;

	void Note(uint64_t start, uint64_t end) {
		if (m_windows.size() < Capacity) {
			m_windows.push_back({start, end});
		} else {
			m_overflow = true;
		}
	}

	void Clear() {
		m_windows.clear();
		m_overflow = false;
	}

	[[nodiscard]] bool Touches(uint64_t start, uint64_t end) const {
		return m_overflow ||
		       std::any_of(m_windows.begin(), m_windows.end(), [&](const CoverageSpan& window) {
			       return window.end >= start && window.start <= end;
		       });
	}

private:
	std::vector<CoverageSpan> m_windows;
	bool                      m_overflow = false;
};

} // namespace Libs::LibKernel::Memory

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_COVERAGE_SPANS_H_ */
