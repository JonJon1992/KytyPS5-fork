#pragma once
#include <cstdint>
namespace Libs::Graphics::ShaderRecompiler::AstroNativeBinding {
// Appended after the diagnostic capture area in the private fault buffer.
// Immutable source words plus a native AS address; never guest memory.
inline constexpr uint32_t Words=512, Data=32, Primitives=288, MaxPrimitives=64;
inline constexpr uint32_t MagicValue=0x42545241, VersionValue=1;
enum Field : uint32_t { Magic, Version, TlasLow, TlasHigh, BaseLow, BaseHigh,
                       Root, ByteCount, PrimitiveCount, Descriptor=9,
                       UsedRays=16, FallbackRays=17 };
}
