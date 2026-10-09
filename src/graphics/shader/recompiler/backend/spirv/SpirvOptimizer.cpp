#include "graphics/shader/recompiler/backend/spirv/SpirvOptimizer.h"

#include "graphics/shader/recompiler/CodegenOptions.h"

#include <spirv-tools/optimizer.hpp>

#include <string_view>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

bool OptimizeProgram(std::vector<uint32_t>& code, std::string& diagnostic) {
	diagnostic.clear();
	if (!GetCodegenOptions().spirv_optimize) {
		return true;
	}

	const auto consume = [&diagnostic](spv_message_level_t, const char*, const spv_position_t&,
	                                    const char* message) {
		if (diagnostic.size() < 2048) {
			diagnostic.append(std::string_view(message).substr(0, 2048 - diagnostic.size()));
			diagnostic += '\n';
		}
	};
	spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
	optimizer.SetMessageConsumer(consume);
	// A fixed, bounded cleanup recipe rather than the evolving -O preset. Keep debug names for
	// diagnostics/Function-array shrinking, all entry-point bindings, output variables and spec
	// constants. No loop unrolling, code motion, float reassociation or relaxed precision.
	optimizer.RegisterPass(spvtools::CreateLocalSingleBlockLoadStoreElimPass());
	optimizer.RegisterPass(spvtools::CreateLocalSingleStoreElimPass());
	optimizer.RegisterPass(spvtools::CreateDeadBranchElimPass());
	optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass(true));
	if (GetCodegenOptions().spirv_optimize_extended) {
		// Study reference: AnyPS5 72cf6c3, SpirvBackend/src/SpirvOptimizer.cpp.
		// Keep the existing cleanup as the baseline. Scalarization enables local load/store
		// forwarding and SSA across blocks. Bound individual aggregate expansion, keep helper
		// calls intact and avoid CCP/Simplification (storage-only narrow types and float rules).
		optimizer.RegisterPass(spvtools::CreateScalarReplacementPass(64));
		optimizer.RegisterPass(spvtools::CreateLocalAccessChainConvertPass());
		optimizer.RegisterPass(spvtools::CreateLocalSingleBlockLoadStoreElimPass());
		optimizer.RegisterPass(spvtools::CreateLocalSingleStoreElimPass());
		optimizer.RegisterPass(spvtools::CreateLocalMultiStoreElimPass());
		optimizer.RegisterPass(spvtools::CreateRedundancyEliminationPass());
		optimizer.RegisterPass(spvtools::CreateDeadBranchElimPass());
		optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass(true));
	}
	optimizer.RegisterPass(spvtools::CreateCFGCleanupPass());
	optimizer.RegisterPass(spvtools::CreateBlockMergePass());
	optimizer.RegisterPass(spvtools::CreateUnifyConstantPass());
	optimizer.RegisterPass(spvtools::CreateRemoveDuplicatesPass());

	spvtools::OptimizerOptions options;
	options.set_run_validator(true);
	// Before SPIR-V 1.4, OpEntryPoint lists only input/output variables. Preserve bindings
	// separately so an unused descriptor outside that list still matches our binding metadata.
	options.set_preserve_bindings(true);
	options.set_preserve_spec_constants(true);
	std::vector<uint32_t> optimized;
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
	validator.SetMessageConsumer(consume);
	// Run validates the original module; validate the final result as well before replacing any
	// words. A failing pass may leave partial/invalid output, which must never reach the driver
	// or the persistent cache. Each call owns its optimizer, so parallel compiles share no state.
	if (!optimizer.Run(code.data(), code.size(), &optimized, options) || !validator.Validate(optimized)) {
		if (diagnostic.empty()) {
			diagnostic = "SPIR-V optimization or output validation failed";
		}
		return false;
	}
	code = std::move(optimized);
	diagnostic.clear();
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
