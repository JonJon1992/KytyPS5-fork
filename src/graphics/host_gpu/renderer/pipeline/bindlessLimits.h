#pragma once

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

// Sampled images per typed array at most. Slots are recycled once no key names their image and
// the commands that could read them completed (BindlessTable::AllocateSlot); the capacity only
// bounds the textures the heaps reference at once (Ghost of Yotei filled 16384 before recycling).
inline constexpr uint32_t BindlessMaxImagesPerArray = 65536u;

struct BindlessBudget {
    uint32_t images = 0;
    uint32_t samplers = 0;
};

// Reserve all six conventional stages' set-0 resources, including 16 storage mips per image.
// The table exposes its arrays to every stage, so both per-stage and aggregate limits apply.
template <typename Properties, typename Limits>
BindlessBudget CalculateBindlessBudget(const Properties& p, const Limits& limits) {
    constexpr uint32_t SetImages = 6u * 64u;
    constexpr uint32_t SetSamplers = 6u * 32u;
    constexpr uint32_t StageBuffers = 64u + 8u;
    constexpr uint32_t StageResources = StageBuffers + 64u * 16u + 64u + 32u;
    const auto spare = [](uint32_t limit, uint32_t reserve) {
        return limit > reserve ? limit - reserve : 0u;
    };
    if (limits.maxBoundDescriptorSets < 2u || limits.maxStorageBufferRange < (1u << 22u) ||
        p.maxDescriptorSetUpdateAfterBindStorageBuffers < 6u * StageBuffers + 2u ||
        p.maxPerStageDescriptorUpdateAfterBindStorageBuffers < StageBuffers + 2u) return {};
    const auto samplers = std::min({4096u,
        spare(p.maxDescriptorSetUpdateAfterBindSamplers, SetSamplers),
        spare(p.maxPerStageDescriptorUpdateAfterBindSamplers, 32u)});
    if (samplers == 0u) return {};
    const auto total_images = std::min({4u * BindlessMaxImagesPerArray,
        spare(p.maxDescriptorSetUpdateAfterBindSampledImages, SetImages),
        spare(p.maxPerStageDescriptorUpdateAfterBindSampledImages, 64u),
        spare(p.maxPerStageUpdateAfterBindResources, StageResources + samplers + 2u),
        // DescriptorHeap::CreateDescriptorPool uses flags=0; set0 consumes none of this
        // update-after-bind pool budget. There is one flagged pool, owned by BindlessTable.
        spare(p.maxUpdateAfterBindDescriptorsInAllPools, samplers + 2u)});
    return total_images / 4u >= 3u ? BindlessBudget {total_images / 4u, samplers}
                                  : BindlessBudget {};
}

} // namespace Libs::Graphics
