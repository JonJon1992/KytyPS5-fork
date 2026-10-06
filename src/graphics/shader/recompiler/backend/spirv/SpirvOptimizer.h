#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_BACKEND_SPIRV_SPIRVOPTIMIZER_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_BACKEND_SPIRV_SPIRVOPTIMIZER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

// Conservative Vulkan 1.3 cleanup, controlled by CodegenOptions::spirv_optimize. Preserves the
// entry-point interface, memory side effects and floating-point precision decorations. Returns
// false with a diagnostic on failure, leaving the original words intact. Disabled is a no-op.
[[nodiscard]] bool OptimizeProgram(std::vector<uint32_t>& code, std::string& diagnostic);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_BACKEND_SPIRV_SPIRVOPTIMIZER_H_
