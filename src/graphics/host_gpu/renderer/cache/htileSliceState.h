#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_HTILESLICESTATE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_HTILESLICESTATE_H_

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

// Fast-clear state of an HTile surface, one bit per array slice: set while a depth clear the host
// attachment has not loaded yet is pending on that slice. A depth view reaches 2048 slices on RDNA2
// (DB_DEPTH_VIEW.SLICE_MAX is 11 bits).
//
// The operations are a clear of every slice (a depth clear), one bit read and one bit changed per
// draw. Slices 0-63 live inline, so the common case never allocates. Words for slices 64 and up
// exist only after one of them differs from `m_high_default`, the value of every slice past the
// stored words: a clear of a 2048-slice array is three stores, not 32.
class HtileSliceState {
public:
	static constexpr uint32_t MaxSlices = 2048;

	HtileSliceState() = default;
	explicit HtileSliceState(bool cleared) { SetAll(cleared); }

	void SetAll(bool cleared) {
		m_low          = cleared ? ~uint64_t {0} : 0;
		m_high_default = cleared;
		m_high.clear();
	}

	[[nodiscard]] bool Test(uint32_t slice) const {
		return slice < MaxSlices && ((Word(slice / 64u) >> (slice % 64u)) & 1u) != 0;
	}

	// The 64 slices [64 * index, 64 * index + 63]; bit i is slice 64 * index + i.
	[[nodiscard]] uint64_t Word(uint32_t index) const {
		if (index == 0) {
			return m_low;
		}
		return index - 1u < m_high.size() ? m_high[index - 1u]
		                                  : (m_high_default ? ~uint64_t {0} : 0);
	}

	// Returns whether the state changed. A slice past MaxSlices is never stored.
	bool Set(uint32_t slice, bool cleared) {
		if (slice >= MaxSlices || Test(slice) == cleared) {
			return false;
		}
		const uint64_t bit = uint64_t {1} << (slice % 64u);
		uint64_t*      word;
		if (slice < 64u) {
			word = &m_low;
		} else {
			const uint32_t high = slice / 64u - 1u;
			if (high >= m_high.size()) {
				m_high.resize(high + 1u, m_high_default ? ~uint64_t {0} : 0);
			}
			word = &m_high[high];
		}
		*word = cleared ? (*word | bit) : (*word & ~bit);
		return true;
	}

	bool operator==(const HtileSliceState& other) const {
		for (uint32_t index = 0; index < MaxSlices / 64u; index++) {
			if (Word(index) != other.Word(index)) {
				return false;
			}
		}
		return true;
	}

private:
	uint64_t              m_low          = 0;
	bool                  m_high_default = false;
	std::vector<uint64_t> m_high;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_HTILESLICESTATE_H_
