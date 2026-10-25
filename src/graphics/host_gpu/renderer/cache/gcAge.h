#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GCAGE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GCAGE_H_

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace Libs::Graphics {

// KYTY_GC_MIN_AGE (default on; =0: ages in frames only, as before). The caches' collectors count
// a resource's age on their LRU clock, which follows presented frames: an age of a few frames is
// a fraction of a second at 60 fps but whole seconds of nothing at 3 fps, and at 3 fps an age of
// one frame freed textures and buffers the game was still using, created again a moment later
// (Wolverine, late in a route, 2026-10-22). With this on, a candidate must also be older than a
// minimum time, converted to clock units at the rate the clock actually moves.
inline bool GcMinAgeEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GC_MIN_AGE");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

// How fast a collector's LRU clock moves, sampled every half second (one per collector; its thread).
class LruClockRate {
public:
	void Sample(uint64_t clock) {
		const auto now = std::chrono::steady_clock::now();
		if (m_sample_clock == 0 && m_rate == 0.0 && m_sample_time == Clock::time_point {}) {
			m_sample_time  = now;
			m_sample_clock = clock;
			return;
		}
		const auto elapsed = now - m_sample_time;
		if (elapsed < std::chrono::milliseconds(500)) {
			return;
		}
		const double seconds = std::chrono::duration<double>(elapsed).count();
		const double rate    = clock >= m_sample_clock ? static_cast<double>(clock - m_sample_clock) / seconds : 0.0;
		m_rate               = m_rate == 0.0 ? rate : (m_rate + rate) / 2.0;
		m_sample_time        = now;
		m_sample_clock       = clock;
	}

	// Clock units in `ms` milliseconds at the measured rate (0 before the first measurement).
	[[nodiscard]] uint64_t UnitsIn(uint32_t ms) const {
		return static_cast<uint64_t>(std::ceil(m_rate * static_cast<double>(ms) / 1000.0));
	}

private:
	using Clock = std::chrono::steady_clock;
	Clock::time_point m_sample_time {};
	uint64_t          m_sample_clock = 0;
	double            m_rate         = 0.0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GCAGE_H_
