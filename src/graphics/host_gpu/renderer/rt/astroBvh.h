#ifndef EMULATOR_GRAPHICS_ASTRO_BVH_H_
#define EMULATOR_GRAPHICS_ASTRO_BVH_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <filesystem>
#include "graphics/host_gpu/renderer/rt/guestBvh.h"

namespace Libs::Graphics::RT {
struct AstroInstance {
	uint64_t address = 0, blas_address = 0;
	uint32_t flags = 0, root = 0;
	std::array<float, 12> world_to_object {}, object_to_world {};
	bool operator==(const AstroInstance&) const = default;
};
// Coherent 160-byte instance record. Embedded TLAS instances use root 37;
// regular instances carry their root at byte 152. Never reads live backing.
std::optional<AstroInstance> DecodeAstroInstance(std::span<const std::byte> bytes,
                                                uint64_t address, bool embedded = false);
// The exact post-BVH test at guest PC 0x538..0x570, for offline comparisons.
// Returns the shader's reciprocal/multiply distance only on an accepted hit.
std::optional<float> AstroAccepts(uint32_t numerator, uint32_t denominator,
                                 uint32_t instance_flags, float extent);
struct CapturedAstroScene { BvhSnapshot bvh; AstroInstance instance; };
// One completed GPU sidecar slot, not an authorization to reuse its bytes in a
// later frame. The current game's memory must match before live replacement.
std::optional<CapturedAstroScene> DecodeCapturedAstroScene(std::span<const uint32_t> node_record,
                                                          std::span<const uint32_t> scene_words,
                                                          bool embedded = false);
// Bounded paired files. Returns an immutable historical source, never a live authorization.
std::optional<CapturedAstroScene> ReadAstroCapture(const std::filesystem::path& nodes,
                                                const std::filesystem::path& scenes);
std::optional<std::array<uint32_t,512>> MakeAstroNativeBinding(const BvhSnapshot& source,
                                                            uint64_t native_address);
} // namespace Libs::Graphics::RT
#endif
