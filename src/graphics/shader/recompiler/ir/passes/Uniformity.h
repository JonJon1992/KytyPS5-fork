#pragma once

#include "graphics/shader/recompiler/ir/Value.h"

#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::IR {

enum class Uniformity : uint8_t { Unknown, Uniform, Divergent };

// Uniform means identical in every guest lane, including lanes outside EXEC.
// This transient proof deliberately excludes memory and control-dependent phis.
// Construct a new analysis after changing the value graph.
class UniformityAnalysis {
public:
	[[nodiscard]] Uniformity Get(Value value) { return Get(value, 0); }

private:
	[[nodiscard]] Uniformity Get(Value value, uint32_t depth);
	[[nodiscard]] Uniformity Classify(const Inst& inst, uint32_t depth);
	std::unordered_map<const Inst*, Uniformity> m_values;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR
