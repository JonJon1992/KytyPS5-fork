#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_

#include <array>
#include <cstdint>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

struct DescriptorValue {
	std::array<uint32_t, 8> dwords      = {};
	uint32_t                dword_count = 0;

	bool operator==(const DescriptorValue& other) const {
		return dword_count == other.dword_count && dwords == other.dwords;
	}
};

enum class UniformFillKind { None, Buffer, Image };

struct UniformFill {
	UniformFillKind          kind         = UniformFillKind::None;
	uint32_t                 resource     = 0;
	std::array<uint32_t, 3> group_stride {};
	uint32_t                 words        = 0;
	uint32_t                 value        = 0;

	bool operator==(const UniformFill&) const = default;
};

struct BindlessHeapUse {
	uint64_t base = 0;
	uint64_t size = 0;
	uint32_t table_offset = 0;
	uint32_t image = 0;
	uint32_t mapping_offset = 0;
	uint32_t record_stride = 32; // bytes between the T#s of consecutive keys
	bool operator==(const BindlessHeapUse&) const = default;
};

struct BindlessSamplerHeapUse {
	uint64_t base = 0;
	uint64_t size = 0;
	uint32_t table_offset = 0;
	uint32_t sampler = 0;
	uint32_t mapping_offset = 0;
	bool operator==(const BindlessSamplerHeapUse&) const = default;
};

struct ResourceSnapshot {
	std::vector<DescriptorValue> buffers;
	std::vector<DescriptorValue> images;
	std::vector<DescriptorValue> samplers;
	std::vector<uint32_t>        flattened_srt;
	std::vector<uint32_t>        user_data;
	std::vector<BindlessHeapUse> bindless_heaps;
	std::vector<BindlessSamplerHeapUse> bindless_sampler_heaps;
	UniformFill                 uniform_fill;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCESNAPSHOT_H_
