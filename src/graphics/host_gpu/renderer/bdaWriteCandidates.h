#ifndef KYTY_BDA_WRITE_CANDIDATES_H_
#define KYTY_BDA_WRITE_CANDIDATES_H_

#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/shader/recompiler/CodegenOptions.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace Libs::Graphics::BdaWriteCandidates {

// Only the two audited shaders. The shadow count is read from the current table, not inferred
// from an observed dispatch. Limits bound host work and allocations, never truncate coverage.
inline constexpr uint32_t MaxDescriptors = 64;
inline constexpr uint32_t MaxTableBytes = 16 + 16 * MaxDescriptors;
inline constexpr uint64_t MaxDestinationBytes = 256ull * 1024 * 1024;
struct Plan {
    uint64_t shader_hash = 0;
    uint64_t table = 0;
    uint64_t vm_generation = 0;
    uint32_t table_bytes = 0;
    uint32_t range_count = 0;
    // Why the dispatch was refused (a string literal; null while the proof holds).
    const char* reject = nullptr;
    // Canonical backing aliases of the table and of each range when Prepare proved them. A guest
    // mapping change elsewhere (a streaming game maps, protects and names memory constantly)
    // leaves the proof valid while these ranges keep their mapping (BufferCache rechecks them).
    uintptr_t table_alias = 0;
    std::array<uintptr_t, MaxDescriptors> range_aliases {};
    std::array<uint32_t, MaxTableBytes / 4> words {};
    std::array<GuestRange, MaxDescriptors> ranges {};
    [[nodiscard]] std::span<const GuestRange> Ranges() const {
        return {ranges.data(), range_count};
    }
};
[[nodiscard]] inline uint32_t PrefixBytes(uint64_t hash) {
    return hash == ShaderRecompiler::BdaWaterLightingHash ? 112u
         : hash == ShaderRecompiler::BdaShadowResolveHash ? 116u : 0u;
}
[[nodiscard]] inline uint32_t TableBytes(uint64_t hash, std::span<const uint32_t> words) {
    const auto prefix = PrefixBytes(hash);
    if (prefix == 0 || words.size_bytes() < prefix) return 0;
    if (hash == ShaderRecompiler::BdaWaterLightingHash) return prefix;
    const auto count = words[0x70 / 4];
    if (count > MaxDescriptors) return 0;
    return std::max(prefix, 16u + count * 16u);
}
// The audited stores are IDXEN DWORD, with zero vector/immediate/scalar byte offsets.
// OOB_SELECT=2 is unbounded; raw OOB_SELECT=3 does not bound a strided index in the emitter.
// Swizzled and short-stride layouts need a separate proof.
[[nodiscard]] inline bool Resolve(Plan& plan) {
    plan.range_count = 0;
    const auto fail = [&](const char* why) {
        plan.reject = why;
        return false;
    };
    const auto bytes = TableBytes(plan.shader_hash, plan.words);
    if ((plan.table & 3u) != 0 || bytes == 0 || bytes != plan.table_bytes ||
        !GuestRange{plan.table, bytes}.Valid()) return fail("table shape or count out of range");
    const auto count = plan.shader_hash == ShaderRecompiler::BdaWaterLightingHash
                           ? 5u : plan.words[0x70 / 4];
    const auto first = plan.shader_hash == ShaderRecompiler::BdaWaterLightingHash ? 32u : 16u;
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        ShaderBufferResource descriptor;
        std::copy_n(plan.words.data() + (first + 16u * i) / 4u, 4, descriptor.fields);
        // These descriptors cannot execute any store in StoreIndirectBuffer.
        if (descriptor.RawFormat() == 0 || descriptor.NumRecords() == 0 ||
            (descriptor.Stride() == 0 && descriptor.OutOfBounds() == 0)) continue;
        if (descriptor.OutOfBounds() == 2 ||
            (descriptor.Stride() != 0 &&
             (descriptor.OutOfBounds() == 3 || descriptor.SwizzleEnabled() ||
              descriptor.Stride() < 4))) return fail("unbounded or swizzled descriptor");
        const auto raw_size = std::max<uint64_t>(4, descriptor.GetSize());
        const auto address = descriptor.Base48() & ~uint64_t{3};
        const auto end = (descriptor.Base48() + raw_size + 3u) & ~uint64_t{3};
        const GuestRange range{address, end - address};
        if (!range.Valid() || range.size > MaxDestinationBytes ||
            total > MaxDestinationBytes - range.size) return fail("destination size limit");
        if (range.address < plan.table + bytes && plan.table < range.End())
            return fail("destination overlaps the table");
        total += range.size;
        plan.ranges[plan.range_count++] = range;
    }
    return true;
}
} // namespace Libs::Graphics::BdaWriteCandidates
#endif
