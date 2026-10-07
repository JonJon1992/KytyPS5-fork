#pragma once

#include <cstdint>

// Diagnostic: where the last resource materialization on this thread failed. Every `return false`
// of SrtWalker.cpp (site 100000 + line) and ResourceMaterialization.cpp (200000 + line) records
// its site; the renderer prints the trail of a stage whose draws it drops.
namespace Libs::Graphics::ShaderRecompiler::IR::MaterializeTrail {

struct Trail {
	static constexpr uint32_t Capacity = 12;
	uint32_t sites[Capacity] {};
	uint32_t count        = 0;
	uint32_t failed_reads = 0;
	uint64_t first_failed_read = 0;
};

inline Trail& Current() noexcept {
	thread_local Trail trail {};
	return trail;
}

inline void Reset() noexcept {
	Current() = {};
}

inline bool Fail(uint32_t site) noexcept {
	auto& trail = Current();
	if (trail.count < Trail::Capacity) {
		trail.sites[trail.count] = site;
	}
	trail.count++;
	return false;
}

inline void FailedRead(uint64_t address) noexcept {
	auto& trail = Current();
	if (trail.failed_reads++ == 0) {
		trail.first_failed_read = address;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::IR::MaterializeTrail

#define KYTY_MATERIALIZE_FAIL(base) \
	(::Libs::Graphics::ShaderRecompiler::IR::MaterializeTrail::Fail((base) + __LINE__))
