#include "graphics/guest_gpu/tile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
using namespace Libs::Graphics;
int failures = 0;
void Check(bool condition, const char* message) {
	if (!condition) { std::fprintf(stderr, "FAILED: %s\n", message); ++failures; }
}
struct Fixture {
	Prospero::BufferFormat format;
	Prospero::TileMode tile;
	uint32_t width, height, depth, levels;
	bool volume;
	uint32_t size, align;
};
constexpr Fixture Fixtures[] = {
	{Prospero::BufferFormat::k32Float, Prospero::TileMode::kStandard64KB, 256, 256, 1, 1, false, 262144, 65536},
	{Prospero::BufferFormat::k32Float, Prospero::TileMode::kStandard64KB, 256, 256, 3, 5, false, 1179648, 65536},
	{Prospero::BufferFormat::k32Float, Prospero::TileMode::kStandard4KB, 33, 17, 1, 1, false, 8192, 4096},
	{Prospero::BufferFormat::k32Float, Prospero::TileMode::kStandard256B, 1, 1, 2, 1, false, 512, 256},
	{Prospero::BufferFormat::k32Float, Prospero::TileMode::kStandard64KB, 1, 1, 17, 1, true, 131072, 65536},
	{Prospero::BufferFormat::k32Float, Prospero::TileMode::kLinear, 65, 3, 2, 1, false, 3072, 256},
	{Prospero::BufferFormat::kBc1UNorm, Prospero::TileMode::kStandard4KB, 33, 17, 3, 4, false, 12288, 4096},
	{Prospero::BufferFormat::kBc1UNorm, Prospero::TileMode::kStandard4KB, 33, 17, 9, 1, true, 16384, 4096},
	{Prospero::BufferFormat::kBc1UNorm, Prospero::TileMode::kLinear, 33, 17, 1, 4, false, 2816, 256},
	{Prospero::BufferFormat::k16UNorm, Prospero::TileMode::kDepth, 257, 129, 1, 1, false, 262144, 65536},
	{Prospero::BufferFormat::k16_16_16_16Float, Prospero::TileMode::kRenderTarget, 129, 65, 1, 1, false, 262144, 65536},
};
void TestSizes() {
	for (const auto& f: Fixtures) {
		TileSizeAlign size {};
		TileGetTextureTotalSize(f.format, f.width, f.height, f.depth, f.levels, f.tile, f.volume, size);
		Check(size.size == f.size && size.align == f.align, "texture total footprint matches hand-derived fixture");
	}
	// Same extent at 1/2/4/8 samples. Derived from the existing 64 KiB block dimensions.
	constexpr uint32_t Pitches[] = {256, 192, 192, 160};
	constexpr uint32_t Sizes[] = {131072, 196608, 393216, 655360};
	for (uint32_t samples_log2 = 0; samples_log2 < 4; ++samples_log2) {
		TileSizeAlign size {};
		const auto pitch = TileGetRenderTargetPitch(129, 4, samples_log2);
		Check(pitch == Pitches[samples_log2] && TileGetDepthPitch(129, 4, samples_log2) == pitch,
		      "MSAA color/depth pitch fixture");
		Check(TileGetRenderTargetSize(129, 65, pitch, 4, size, samples_log2) &&
		          size.size == Sizes[samples_log2] && size.align == 65536,
		      "MSAA footprint fixture");
	}
}
void Bench() {
	constexpr uint32_t Repetitions = 20000;
	uint64_t checksum = 0;
	const auto start = std::chrono::steady_clock::now();
	for (uint32_t rep = 0; rep < Repetitions; ++rep) {
		for (uint32_t i = 0; i < 32; ++i) {
			TileSizeAlign size {};
			TileGetTextureTotalSize(Prospero::BufferFormat::k32Float, 1920 + i, 1080 + i,
			                        6, 11, Prospero::TileMode::kStandard64KB, false, size);
			checksum += size.size + size.align;
		}
	}
	const double elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
	std::printf("tile-size-bench: %.3f ns/texture checksum=%" PRIu64 "\n", elapsed / (Repetitions * 32), checksum);
}

void TestTailBoundaries() {
	constexpr Prospero::BufferFormat Formats[] = {Prospero::BufferFormat::k8UNorm,
		Prospero::BufferFormat::k16UNorm, Prospero::BufferFormat::k32Float,
		Prospero::BufferFormat::k16_16_16_16Float, Prospero::BufferFormat::k32_32_32_32Float,
		Prospero::BufferFormat::kBc1UNorm, Prospero::BufferFormat::kBc3UNorm};
	constexpr Prospero::TileMode Modes[] = {Prospero::TileMode::kStandard4KB,
		Prospero::TileMode::kStandard64KB, Prospero::TileMode::kPrt,
		Prospero::TileMode::kRenderTarget, Prospero::TileMode::kDepth};
	// The full layout retains the original mip-tail search. Cross-check the size-only path
	// around powers of two, with all mip counts, block sizes, element sizes and both dimensions.
	for (auto format: Formats) for (auto mode: Modes) for (bool volume: {false, true})
	for (uint32_t power = 0; power <= 13; ++power) for (int32_t delta: {-1, 0, 1})
	for (uint32_t levels = 1; levels <= 16; ++levels) {
		const auto width = std::max(static_cast<int32_t>(1u << power) + delta, 1);
		const auto height = std::max(static_cast<int32_t>(1u << ((power + 3) % 14)) - delta, 1);
		const TileSurfaceDescription desc {format, mode,
			volume ? TileSurfaceDimension::Dim3D : TileSurfaceDimension::Dim2D,
			static_cast<uint32_t>(width), static_cast<uint32_t>(height), volume ? 17u : 1u,
			levels, volume ? 1u : 6u};
		TileSurfaceLayout reference {};
		if (!TileGetTiledTextureLayout(desc, reference) || reference.total_size > UINT32_MAX) continue;
		TileSizeAlign size {};
		TileGetTextureTotalSize(format, desc.width, desc.height, volume ? desc.depth : desc.layers,
		                        levels, mode, volume, size);
		Check(size.size == reference.total_size && size.align == reference.texture.block.block_size,
		      "size-only mip tail matches full layout around power-of-two boundaries");
	}
}

void BenchMatrix() {
	constexpr Prospero::BufferFormat Formats[] = {Prospero::BufferFormat::k32Float,
		Prospero::BufferFormat::k8UNorm, Prospero::BufferFormat::k16_16_16_16Float,
		Prospero::BufferFormat::kBc1UNorm};
	constexpr Prospero::TileMode Modes[] = {Prospero::TileMode::kStandard256B,
		Prospero::TileMode::kStandard4KB, Prospero::TileMode::kStandard64KB, Prospero::TileMode::kPrt};
	constexpr const char* Names[] = {"repeated", "cube-layers", "cycling32", "unique-extents",
		"mixed-geometry", "one-mip", "volume", "linear", "full-layout", "descriptor-path", "descriptor-repeated"};
	constexpr uint32_t Repetitions = 100000;
	for (uint32_t scenario = 0; scenario < std::size(Names); ++scenario) {
		uint64_t checksum = 0;
		const auto start = std::chrono::steady_clock::now();
		for (uint32_t rep = 0; rep < Repetitions; ++rep) for (uint32_t i = 0; i < 32; ++i) {
			const auto index = scenario < 2 || scenario == 10 ? 0u : scenario == 3 ? ((rep * 32 + i) & 4095u) : i;
			const auto format = scenario == 4 ? Formats[i & 3u] : Formats[0];
			const auto mode = scenario == 7 ? Prospero::TileMode::kLinear : scenario == 4 ? Modes[(i >> 2u) & 3u] : Modes[2];
			const auto levels = scenario == 5 ? 1u : 11u;
			const auto volume = scenario == 6;
			const auto layers = volume ? 17u + i : scenario == 1 ? (i % 3 + 1u) * 6u : 6u;
			if (scenario == 8) {
				TileSurfaceLayout layout {};
				TileGetTiledTextureLayout({format, mode, TileSurfaceDimension::Dim2D,
				    1920 + index, 1080 + index, 1, levels, layers}, layout);
				checksum += layout.total_size + layout.texture.block.block_size;
			} else {
				TileSizeAlign size {};
				// The descriptor path computes pitch before size; keep both calls in this measurement.
				if (scenario >= 9) checksum += TileGetTexturePitch(format, 1920 + index, mode);
				TileGetTextureTotalSize(format, 1920 + index, 1080 + index, layers, levels, mode, volume, size);
				checksum += size.size + size.align;
			}
		}
		const double elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
		std::printf("tile-matrix %s: %.3f ns/texture checksum=%" PRIu64 "\n", Names[scenario], elapsed / (Repetitions * 32), checksum);
	}
}

void Snapshot(const char* path) {
	auto* file = std::fopen(path, "wb");
	if (file == nullptr) { Check(false, "open snapshot"); return; }
	const auto write = [&](std::array<uint64_t, 8> words) {
		Check(std::fwrite(words.data(), sizeof(words), 1, file) == 1, "write snapshot");
	};
	constexpr Prospero::TileMode Modes[] = {Prospero::TileMode::kLinear, Prospero::TileMode::kStandard256B,
		Prospero::TileMode::kStandard4KB, Prospero::TileMode::kStandard64KB, Prospero::TileMode::kPrt,
		Prospero::TileMode::kRenderTarget, Prospero::TileMode::kDepth};
	constexpr std::array<uint32_t, 2> Extents[] = {{1, 1}, {33, 17}, {128, 129}, {255, 257}, {1921, 1081}};
	uint32_t cases = 0, valid = 0;
	for (uint32_t fmt = 0; fmt <= 183; ++fmt) for (auto mode: Modes) for (auto extent: Extents)
	for (uint32_t levels: {1u, 5u, 16u}) for (bool volume: {false, true}) for (uint32_t layers: {1u, 6u, 12u}) {
		const auto format = static_cast<Prospero::BufferFormat>(fmt);
		TileSurfaceDescription desc {format, mode, volume ? TileSurfaceDimension::Dim3D : TileSurfaceDimension::Dim2D,
			extent[0], extent[1], volume ? 17u : 1u, levels, volume ? 1u : layers};
		TileSurfaceLayout layout {};
		TileTextureElementLayout element {};
		const bool linear = mode == Prospero::TileMode::kLinear;
		const bool supported = linear ? TileGetTextureElementLayout(format, element) : TileGetTiledTextureLayout(desc, layout);
		write({fmt, static_cast<uint32_t>(mode), extent[0], extent[1], levels, volume, layers, supported});
		++cases;
		if (!supported) continue;
		++valid;
		TileSizeAlign total {};
		TileGetTextureTotalSize(format, extent[0], extent[1], volume ? 17u : layers, levels, mode, volume, total);
		write({total.size, total.align, TileGetTexturePitch(format, extent[0], mode), layout.total_size,
			layout.block_slice_size, layout.first_tail_level, 0, 0});
		for (const auto& mip: layout.mips) {
			write({mip.offset, mip.size, mip.width, mip.height, mip.padded_width, mip.padded_height, mip.tail_x, mip.tail_y});
		}
		if (linear || !volume) {
			TileSizeOffset offsets[16] {};
			TilePaddedSize pads[16] {};
			TileGetTextureSize(format, extent[0], extent[1], levels, mode, &total, offsets, pads);
			write({total.size, total.align, 0, 0, 0, 0, 0, 0});
			for (uint32_t l = 0; l < levels; ++l) {
				const auto& mip = offsets[l];
				write({mip.size, mip.offset, mip.src_size, mip.src_offset, mip.x, mip.y, pads[l].width, pads[l].height});
			}
		}
	}
	// Exhaust the existing MSAA block table and its metadata sizing with several layer counts.
	for (uint32_t bytes: {1u, 2u, 4u, 8u, 16u}) for (uint32_t samples = 0; samples <= 4; ++samples)
	for (auto extent: Extents) for (uint32_t layers: {1u, 6u, 12u}) {
		const auto pitch = TileGetRenderTargetPitch(extent[0], bytes, samples);
		TileSizeAlign size {}, dcc {};
		const bool ok = TileGetRenderTargetSize(extent[0], extent[1], pitch, bytes, size, samples);
		const bool meta = TileGetDccSize(extent[0], extent[1], layers, bytes, 1, Prospero::TileMode::kRenderTarget, dcc, samples);
		write({pitch, TileGetDepthPitch(extent[0], bytes, samples), ok, size.size, size.align, meta, dcc.size, dcc.align});
		TileSizeAlign stencil {}, htile {}, depth {};
		if (samples <= 3) {
			const bool depth_ok = TileGetDepthSize(extent[0], extent[1], 0, Prospero::DepthFormat::kZ32F,
			    Prospero::StencilFormat::k8UInt, true, stencil, htile, depth, samples);
			write({depth_ok, stencil.size, stencil.align, htile.size, htile.align, depth.size, depth.align, 0});
		}
	}
	Check(std::fclose(file) == 0, "close snapshot");
	std::printf("tile snapshot: cases=%u valid=%u\n", cases, valid);
}
}
int main(int argc, char** argv) {
	TestSizes();
	TestTailBoundaries();
	if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) Bench();
	if (argc > 1 && std::strcmp(argv[1], "--bench-matrix") == 0) BenchMatrix();
	if (argc > 2 && std::strcmp(argv[1], "--snapshot") == 0) Snapshot(argv[2]);
	if (!failures) std::puts("Tile size fixtures passed");
	return failures != 0;
}
