#ifndef EMULATOR_GRAPHICS_SHADER_RECOMPILER_BVHCAPTURE_H_
#define EMULATOR_GRAPHICS_SHADER_RECOMPILER_BVHCAPTURE_H_

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::BvhCapture {

// Diagnostic storage appended after both fault bitmaps, never guest GDS.
// One capture is one dispatch. GPU completion publishes the header and records.
inline constexpr uint32_t Magic = 0x43564842; // "BHVC", little endian
inline constexpr uint32_t Version = 2;
inline constexpr uint32_t HeaderWords = 64;
inline constexpr uint32_t RecordWords = 64;
inline constexpr uint32_t MaxRecords = 65536;
inline constexpr uint64_t AstroShader = 0x7d6ab87e6c984bb7ull;
inline constexpr uint32_t SceneOffset = HeaderWords + MaxRecords * RecordWords;
inline constexpr uint32_t SceneWords = 320, MaxScenes = 64;
inline constexpr uint64_t SceneBytes = uint64_t(SceneWords) * MaxScenes * 4;
inline constexpr uint64_t Bytes = uint64_t(SceneOffset) * 4 + SceneBytes;

enum Header : uint32_t {
	Count, Enabled, FileMagic, FileVersion, Stride, Capacity,
	ShaderLow, ShaderHigh, GroupsX, GroupsY, GroupsZ, Mode, TickLow, TickHigh,
	Indirect, SampleMask, // 0 records every invocation; 2^n-1 samples one in 2^n.
	PcFilter, // Optional reserved header word: 0 captures every guest PC.
	SceneEnabled // Astro-only full BLAS/instance sidecar, default off.
};
enum Record : uint32_t {
	Pc = 0, NodeLow = 1, NodeHigh = 2, NodeWords = 3, Descriptor = 4,
	Extent = 8, Origin = 9, Direction = 12, Inverse = 15, Result = 18,
	Data = 22, Invocation = 54, Status = 57,
	InstanceLow = 58, InstanceHigh = 59, LeafId = 60, InstanceFlags = 61
};
enum NodeStatus : uint32_t { Resident = 1, InvalidInput = 2, Unmapped = 3 };
enum Scene : uint32_t {
	SceneStatus = 0, SceneVersion = 1, BlasBytes = 2, BlasLow = 3, BlasHigh = 4,
	SceneInstanceLow = 5, SceneInstanceHigh = 6, SceneLeafId = 7, SceneFlags = 8,
	ScenePc = 9, InstanceData = 16, BlasData = 64
};
enum SceneStatus : uint32_t { CompleteScene = 1, MissingScene = 2, LargeScene = 3, InvalidScene = 4 };

} // namespace Libs::Graphics::ShaderRecompiler::BvhCapture
#endif
