#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Libs::Graphics {

// known_tick must include both GPU completion and host submission retirement.
template <typename Refresh>
std::optional<size_t> FindReusableCommandSlot(std::span<const uint64_t> ticks, size_t hint,
                                            uint64_t known_tick, Refresh&& refresh) {
	const auto search = [&](size_t begin, size_t end) -> std::optional<size_t> {
		for (size_t i = begin; i < end; ++i) {
			if (ticks[i] <= known_tick) {
				return i;
			}
		}
		return std::nullopt;
	};
	if (ticks.empty()) {
		return std::nullopt;
	}
	auto found = search(hint, ticks.size());
	if (!found) {
		found = search(0, hint);
	}
	if (!found) {
		known_tick = refresh();
		found = search(hint, ticks.size());
		if (!found) {
			found = search(0, hint);
		}
	}
	return found;
}

} // namespace Libs::Graphics
