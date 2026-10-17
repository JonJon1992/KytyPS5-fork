#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHRUN_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHRUN_H_

#include <array>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.hpp>

namespace Libs::Graphics {

// Dispatch runs (KYTY_DISPATCH_RUNS=1). A queue's command processor launches back-to-back
// dispatches without waiting for one another; the game orders them only where its command
// stream asks for it (EVENT_WRITE CS_PARTIAL_FLUSH, ACQUIRE_MEM, RELEASE_MEM, WAIT_REG_MEM, ...).
// The renderer put a full barrier before and after every dispatch instead, so the async compute
// chains of hundreds of tiny dispatches ran one at a time with the GPU draining between them
// (the title: ~640 indirect dispatches of 0xbad108e74fb72e9f, 28 ms of GPU time, plus ~25 ms of
// gaps, a frame). A run is a sequence of dispatches separated only by SET_SH_REG and debug-marker
// packets: inside it the barriers between the dispatches are left out, the thread counts of its
// indirect dispatches are converted together, and the barrier after the run's last dispatch
// orders the whole run for what follows.
struct DispatchRunIndirect {
	const uint32_t*         packet = nullptr;
	uint64_t                args   = 0;
	std::array<uint32_t, 3> local {}; // 0: the value current at the run's first dispatch
};

struct DispatchRunConverted {
	vk::Buffer        groups_buffer;
	vk::DeviceSize    groups_offset = 0;
	vk::DeviceAddress threads       = 0;
};

struct DispatchRunState {
	const uint32_t* packet    = nullptr; // the dispatch being recorded
	bool            continues = false;   // an earlier dispatch of the same run was recorded last
	bool            more      = false;   // another dispatch of the run follows
	// The run's upcoming thread-dimension indirect dispatches (from this one), collected by the
	// command processor when this one has no converted counts yet.
	std::vector<DispatchRunIndirect>                                  upcoming;
	std::unordered_map<const uint32_t*, DispatchRunConverted>         converted;
	bool pending_post = false; // a dispatch of the run left out its barrier after it
	uint64_t runs = 0, members = 0, batched = 0, skipped_barriers = 0; // stats
};

inline bool DispatchRunsEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_DISPATCH_RUNS");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

// GPU thread only.
inline DispatchRunState& DispatchRun() {
	static DispatchRunState state;
	return state;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHRUN_H_
