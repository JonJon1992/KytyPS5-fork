#include "graphics/host_gpu/renderer/commandPoolReuse.h"

#include <array>
#include <cstdio>

int main() {
	using Libs::Graphics::FindReusableCommandSlot;
	int failures = 0, refreshes = 0;
	const auto check = [&](bool condition, const char* message) {
		if (!condition) { std::printf("FAILED: %s\n", message); ++failures; }
	};
	std::array<uint64_t, 4> ticks {1, 9, 9, 9};
	auto found = FindReusableCommandSlot(ticks, 3, 1, [&] { ++refreshes; return 1; });
	check(found == 0 && refreshes == 0, "known completed prefix is reused without a driver query");
	refreshes = 0;
	found = FindReusableCommandSlot({}, 0, 0, [&] { ++refreshes; return 0; });
	check(!found && refreshes == 0, "empty pool grows without a progress query");
	refreshes = 0;
	ticks = {9, 5, 9, 5};
	found = FindReusableCommandSlot(ticks, 2, 5, [&] { ++refreshes; return 5; });
	check(found == 3 && refreshes == 0, "known free tail preserves rotation preference");
	ticks = {7, 7, 7, 7};
	found = FindReusableCommandSlot(ticks, 2, 6, [&] { ++refreshes; return 7; });
	check(found == 2 && refreshes == 1, "busy pool refreshes exactly once before reusing completed work");
	refreshes = 0;
	found = FindReusableCommandSlot(ticks, 2, 6, [&] { ++refreshes; return 6; });
	check(!found && refreshes == 1, "in-flight slots remain unavailable after a stalled refresh");
	refreshes = 0;
	found = FindReusableCommandSlot(ticks, 2, 6, [&] { ++refreshes; ticks[0] = 6; return 6; });
	check(found == 0 && refreshes == 1, "refreshed search covers the prefix as well as the tail");
	if (!failures) std::puts("Command pool: retirement, rotation and minimal progress queries passed");
	return failures != 0;
}
