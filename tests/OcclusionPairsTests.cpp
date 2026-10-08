#include "graphics/host_gpu/renderer/occlusionPairs.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using Libs::Graphics::OcclusionDumpPairs;
using Kind = OcclusionDumpPairs::Kind;

static void Require(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "occlusion pairs: %s\n", message);
		std::abort();
	}
}

int main() {
	OcclusionDumpPairs pairs;
	Require(pairs.Observe(0x1000) == Kind::Begin, "aligned begin");
	Require(pairs.Observe(0x1008) == Kind::End, "aligned end");
	Require(pairs.Empty(), "aligned pair closed");

	// Astro Bot also starts a pair at A % 16 == 8; its end is 16-byte aligned.
	Require(pairs.Observe(0x2008) == Kind::Begin, "shifted begin");
	Require(pairs.Observe(0x2100) == Kind::Begin, "nested begin");
	Require(pairs.Observe(0x2108) == Kind::End, "nested end");
	Require(pairs.Observe(0x2010) == Kind::End, "shifted end");
	Require(pairs.Empty(), "nested pairs closed");

	Require(pairs.Observe(0x3008) == Kind::Begin, "repeated begin initial");
	Require(pairs.Observe(0x3008) == Kind::RepeatedBegin, "repeated begin");
	Require(pairs.Observe(0x3010) == Kind::End, "end after repeated begin");

	// An end can be recognised only while its corresponding begin is still open.
	Require(pairs.Observe(0) == Kind::Begin, "zero address does not underflow");
	Require(pairs.Observe(8) == Kind::End, "zero address pair ends");
	Require(pairs.Observe(0x4000) == Kind::Begin, "stale begin");
	for (uint64_t i = 0; i < OcclusionDumpPairs::MaxAgeDumps / 2; ++i) {
		Require(pairs.Observe(0x9000) == Kind::Begin, "filler begin");
		Require(pairs.Observe(0x9008) == Kind::End, "filler end");
	}
	Require(pairs.Dropped() == 1, "stale pair expired");
	Require(pairs.Observe(0x4008) == Kind::Begin, "stale end starts a new pair");
	Require(pairs.Observe(0x4010) == Kind::End, "new pair closes");

	std::puts("occlusion pairs: ok");
	return 0;
}
