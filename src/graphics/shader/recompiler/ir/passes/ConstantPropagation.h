#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

void ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size = 64);

struct Program;
// Also replaces provably masked local-aperture global loads with zero.
uint32_t SimplifyLocalAddressStores(Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR
