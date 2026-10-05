#pragma once

#include <chrono>
#include <future>

namespace Libs::Graphics {

// Called under the pending-map mutex, only when speculative storage is full.
// A moved future reserves the key for its consuming draw and must never be retired.
template <typename PendingMap>
auto FindCompletedPrefetchToRetire(PendingMap& pending) {
	auto oldest = pending.end();
	for (auto it = pending.begin(); it != pending.end(); ++it) {
		const auto& job = it->second;
		if (job.required || !job.future.valid() ||
		    job.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
		if (oldest == pending.end() || job.ticket < oldest->second.ticket) oldest = it;
	}
	return oldest;
}

} // namespace Libs::Graphics
