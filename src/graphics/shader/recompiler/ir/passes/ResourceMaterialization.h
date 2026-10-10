#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <span>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Canonical module-affecting resource state. Runtime addresses and descriptor payloads remain in
// ResourceSnapshot and therefore do not create shader permutations.
struct ResourceSpecialization {
	struct Buffer {
		uint32_t               packed_stride                   = 0;
		Prospero::BufferFormat descriptor_format               = Prospero::BufferFormat::kInvalid;
		uint32_t               descriptor_swizzle              = DstSel(4, 5, 6, 7);
		bool                   zero_stride_oob                 = false;
		bool                   readonly_safe                   = false;
		bool                   operator==(const Buffer&) const = default;
	};

	struct Image {
		Prospero::TextureNumericClass numeric_class = Prospero::TextureNumericClass::Unsupported;
		Decoder::ImageDimension       dimension     = Decoder::ImageDimension::Unknown;
		uint32_t                      mip_count     = 1;
		Prospero::BufferFormat        conversion_format          = Prospero::BufferFormat::kInvalid;
		uint32_t                      shader_swizzle             = ShaderImageIdentitySwizzle;
		uint32_t                      indirect_root              = ImageResource::NoIndirectImage;
		uint32_t                      indirect_mapping_offset    = 0;
		uint32_t                      indirect_search_iterations = 0;
		bool                          cube                       = false;
		bool                          fmask                      = false;
		bool                          bindless                   = false;
		bool                          operator==(const Image&) const = default;
	};

	struct Sampler {
		bool bindless = false;
		uint32_t bindless_mapping_offset = 0;
		bool operator==(const Sampler&) const = default;
	};

	std::vector<Buffer> buffers;
	std::vector<Image>  images;
	std::vector<Sampler> samplers;

	bool operator==(const ResourceSpecialization&) const = default;
};

// Extracts the descriptor/SRT value graph before resource specialization. The returned plan owns
// its values and is independent of the translated shader CFG.
ResourcePlan ExtractResourcePlan(const Program& program);

// Refreshes cached resources and specialization in place. A failed refresh must not be used.
// Threads may refresh one sealed plan concurrently when each supplies its own scratch.
bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          EvaluationScratch& scratch, ResourceSnapshot& snapshot,
                          ResourceSpecialization& specialization);
// Uses this thread's scratch.
bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization);

// Writes the stride of every PackedStrideRuntime buffer, from its descriptor in `descriptors`
// (the snapshot's), into the zeroed stride region of `shader_data` (BindingLayout).
void WriteBufferStrides(const BindingLayout& layout, std::span<const BufferResource> buffers,
                        std::span<const DescriptorValue> descriptors,
                        std::span<uint32_t>              shader_data);

// Whether `specialization` has the shape ApplyResourceSpecialization requires of a program whose
// resources are `info`: one entry per buffer and at least one per image. A specialization
// materialized from the program's own plan always fits; a stored one (the shader journal) may
// come from another translator.
[[nodiscard]] inline bool SpecializationFits(const ShaderInfo&             info,
                                             const ResourceSpecialization& specialization) {
	return info.buffers.size() == specialization.buffers.size() &&
	       info.images.size() <= specialization.images.size();
}

// Applies an already-derived specialization to native IR before layout and emission.
void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_ */
