#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

struct FunctionLdsLayout {
	std::unordered_map<const Inst*, uint32_t> slots;
	uint32_t                                 dwords = 0;
};

// Adapted from the demon donor's FunctionLdsLayout.h at
// f17993c1f7af06b622887fe5c496517cd690e35c.
// Pixel LDS already uses invocation-private Function storage. For scalar accesses
// at (LaneId << 2) + an aligned offset, the lane term is identical within each
// invocation. Offsets identify cells modulo the emitter's 16-bit DS address mask.
// Keep the original address and bounds checks; this only changes storage indices.
// Any incompatible LDS access rejects the entire layout, preserving all aliases.
inline FunctionLdsLayout PlanFunctionLdsLayout(const Program& program) {
	FunctionLdsLayout result;
	if (program.stage != ShaderType::Pixel) return result;
	std::vector<uint32_t>                     offsets;
	std::unordered_map<const Inst*, uint32_t> accesses;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (SharedAccessOf(inst.GetOpcode()) == SharedAccess::None) continue;
			const auto index = inst.Flags<MemoryFlags>().index;
			if (index >= program.memory_info.size()) return {};
			const auto& memory = program.memory_info[index];
			if (memory.kind != ResourceKind::Lds) continue;
			if ((inst.GetOpcode() != ValueOpcode::LoadSharedU32 &&
			     inst.GetOpcode() != ValueOpcode::WriteSharedU32) ||
			    memory.offset % 4u != 0u) {
				return {};
			}
			const auto* address = inst.Arg(0).Resolve().TryInstruction();
			if (!address || address->GetOpcode() != ValueOpcode::ShiftLeftLogical32) return {};
			const auto  shift = address->Arg(1).Resolve();
			const auto* lane  = address->Arg(0).Resolve().TryInstruction();
			if (!shift.IsImmediate() || shift.GetType() != Type::U32 || shift.U32() != 2u ||
			    !lane || lane->GetOpcode() != ValueOpcode::LaneId) {
				return {};
			}
			const auto offset = memory.offset & 0xffffu;
			accesses.emplace(&inst, offset);
			offsets.push_back(offset);
		}
	}
	std::sort(offsets.begin(), offsets.end());
	offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
	// The original pixel Function array has 8192 dwords. The 16-bit DS address
	// space can describe more distinct offsets, but compaction must reduce storage.
	if (offsets.size() >= 8192u) return {};
	for (const auto& [inst, offset]: accesses) {
		result.slots.emplace(
		    inst, static_cast<uint32_t>(std::lower_bound(offsets.begin(), offsets.end(), offset) -
		                                offsets.begin()));
	}
	result.dwords = static_cast<uint32_t>(offsets.size());
	return result;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
