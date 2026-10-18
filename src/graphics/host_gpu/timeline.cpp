#include "graphics/host_gpu/timeline.h"

#include "common/logging/log.h"
#include "kernel/pthread.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs::Graphics::Timeline {

namespace {

struct Window {
	uint64_t first = 0;
	uint64_t count = 0;
};

const Window& GetWindow() {
	static const Window window = [] {
		Window result;
		const char* value = std::getenv("KYTY_TIMELINE");
		if (value != nullptr) {
			char* end    = nullptr;
			result.first = std::strtoull(value, &end, 10);
			result.count = end != nullptr && *end == ':' ? std::strtoull(end + 1, nullptr, 10) : 1;
		}
		return result;
	}();
	return window;
}

uint64_t NowMicros() {
	static const auto start = std::chrono::steady_clock::now();
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	                                 std::chrono::steady_clock::now() - start)
	                                 .count());
}

uint32_t ThreadId() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return static_cast<uint32_t>(GetCurrentThreadId());
#else
	return 0;
#endif
}

} // namespace

namespace Detail {

void MarkSlow(const char* event, uint64_t a, uint64_t b) {
	// Each thread names itself once (guest thread name, or host).
	thread_local bool named = false;
	if (!named) {
		named              = true;
		char name[64]      = "(host)";
		if (auto self = Libs::LibKernel::PthreadSelfOrNull(); self != nullptr) {
			if (Libs::LibKernel::PthreadGetname(self, name) != 0) {
				std::snprintf(name, sizeof(name), "(guest)");
			}
		}
		std::fprintf(stderr, "TLN %u %s\n", ThreadId(), name);
	}
	// stderr: the emulator log is usually silent.
	std::fprintf(stderr, "TL %" PRIu64 " %u %s 0x%" PRIx64 " 0x%" PRIx64 "\n", NowMicros(),
	             ThreadId(), event, a, b);
}

} // namespace Detail

void Present(uint64_t frame) {
	const auto& window = GetWindow();
	if (window.count == 0) {
		return;
	}
	const bool active = frame >= window.first && frame - window.first < window.count;
	if (active || Detail::g_active.load(std::memory_order_relaxed)) {
		Detail::MarkSlow("present", frame, 0);
	}
	Detail::g_active.store(active, std::memory_order_relaxed);
}

} // namespace Libs::Graphics::Timeline
