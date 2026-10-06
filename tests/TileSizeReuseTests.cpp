#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/tile.h"

#include <cstdio>
#include <thread>

namespace {
thread_local uint32_t format_queries = 0;
int failures = 0;
void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FAILED: %s\n", message);
		++failures;
	}
}
}

namespace Libs::Graphics::Prospero {
// The test links the real gpu_format.cpp with just this symbol renamed. Count external format
// decoding without instrumenting production tile.cpp or replacing any format facts.
uint32_t TileSizeUncachedNumBytesPerElement(BufferFormat format);
uint32_t NumBytesPerElement(BufferFormat format) {
	++format_queries;
	return TileSizeUncachedNumBytesPerElement(format);
}
}

int main() {
	using namespace Libs::Graphics;
	TileSizeAlign size {};
	for (uint32_t i = 0; i < 32; ++i) {
		TileGetTextureTotalSize(Prospero::BufferFormat::k32Float, 1920 + i, 1080 + i,
		                        6, 11, Prospero::TileMode::kStandard64KB, false, size);
	}
	std::printf("format queries for 32 extents: %u\n", format_queries);
	Check(format_queries == 1, "same tiled geometry decodes immutable format facts only once");
	TileGetTextureTotalSize(Prospero::BufferFormat::k32Float, 256, 256, 1, 1,
	                        Prospero::TileMode::kStandard64KB, false, size);
	Check(size.size == 262144 && size.align == 65536, "single-layer footprint");
	for (uint32_t layers: {6u, 12u, 36u}) {
		TileGetTextureTotalSize(Prospero::BufferFormat::k32Float, 256, 256, layers, 1,
		                        Prospero::TileMode::kStandard64KB, false, size);
		Check(size.size == 262144u * layers && size.align == 65536, "cube/array layer count is applied on every call");
	}
	TileGetTextureTotalSize(Prospero::BufferFormat::k32Float, 1, 1, 17, 1,
	                        Prospero::TileMode::kStandard64KB, true, size);
	Check(size.size == 131072 && size.align == 65536, "volume depth rounds to block slices");
	TileGetTextureTotalSize(Prospero::BufferFormat::k32Float, 1, 1, 33, 1,
	                        Prospero::TileMode::kStandard64KB, true, size);
	Check(size.size == 196608 && size.align == 65536, "volume depth is part of the footprint key");
	TileGetTextureTotalSize(Prospero::BufferFormat::k32Float, 1, 1, 17, 1,
	                        Prospero::TileMode::kStandard64KB, false, size);
	Check(size.size == 1114112 && size.align == 65536, "volume and array footprints stay distinct");
	TileGetTextureTotalSize(Prospero::BufferFormat::k32Float, 1, 1, 1, 1,
	                        Prospero::TileMode::kLinear, false, size);
	Check(size.size == 256 && size.align == 256, "linear and tiled footprints stay distinct");
	TileGetTextureTotalSize(Prospero::BufferFormat::k32Float, 256, 256, 1, 5,
	                        Prospero::TileMode::kStandard64KB, false, size);
	Check(size.size == 393216 && size.align == 65536, "mip count is part of the footprint key");
	TileGetTextureTotalSize(Prospero::BufferFormat::k8UNorm, 256, 256, 1, 1,
	                        Prospero::TileMode::kStandard64KB, false, size);
	Check(size.size == 65536 && size.align == 65536, "element size is part of the footprint key");
	// A fresh worker must decode its own geometry, and cannot overwrite the parent's reuse state.
	bool isolated = false;
	std::thread worker([&] {
		TileSizeAlign other {};
		for (uint32_t i = 0; i < 16; ++i) {
			TileGetTextureTotalSize(Prospero::BufferFormat::k8UNorm, 256, 256, 1, 1,
			                        Prospero::TileMode::kStandard64KB, false, other);
		}
		isolated = format_queries == 1 && other.size == 65536 && other.align == 65536;
	});
	worker.join();
	Check(isolated, "reuse state is independent for each thread");
	format_queries = 0;
	TileGetTextureTotalSize(Prospero::BufferFormat::k8UNorm, 256, 256, 2, 1,
	                        Prospero::TileMode::kStandard64KB, false, size);
	Check(format_queries == 0 && size.size == 131072, "worker lookup leaves parent footprint reusable");
	if (!failures) std::puts("Tile footprint reuse tests passed");
	return failures != 0;
}
