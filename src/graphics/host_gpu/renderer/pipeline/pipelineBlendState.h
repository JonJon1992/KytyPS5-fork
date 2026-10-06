#pragma once

#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

namespace Libs::Graphics {

// Vulkan MIN/MAX operate on source/destination components directly, without factors.
// Guest blend op encodings differ from VkBlendOp; use the guest enum here.
inline void NormalizeMinMaxBlendFactors(PipelineStaticParameters& params) {
	const auto ignores_factors = [](uint8_t op) {
		return op == static_cast<uint8_t>(Prospero::BlendOp::kMin) ||
		       op == static_cast<uint8_t>(Prospero::BlendOp::kMax);
	};
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; ++slot) {
		if (!params.blend_enable[slot]) continue;
		if (ignores_factors(params.color_comb_fcn[slot])) {
			params.color_srcblend[slot] = 0;
			params.color_destblend[slot] = 0;
		}
		if (params.separate_alpha_blend[slot] && ignores_factors(params.alpha_comb_fcn[slot])) {
			params.alpha_srcblend[slot] = 0;
			params.alpha_destblend[slot] = 0;
		}
	}
}

} // namespace Libs::Graphics
