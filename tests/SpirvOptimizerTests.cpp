#include "graphics/shader/recompiler/CodegenOptions.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvOptimizer.h"

#include <spirv-tools/libspirv.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace Recompiler = Libs::Graphics::ShaderRecompiler;
int failures = 0;

void Expect(bool condition, const char* message) {
	if (!condition) {
		std::printf("SpirvOptimizerTests: failed: %s\n", message);
		++failures;
	}
}

std::vector<uint32_t> Assemble(const std::string& text, spv_target_env env = SPV_ENV_VULKAN_1_3) {
	spvtools::SpirvTools tools(env);
	std::vector<uint32_t> code;
	tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
		std::printf("SPIR-V fixture: %s\n", message);
	});
	Expect(tools.Assemble(text, &code), "fixture assembles");
	Expect(tools.Validate(code), "fixture validates");
	return code;
}

std::string Disassemble(const std::vector<uint32_t>& code) {
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string text;
	Expect(tools.Validate(code), "optimized module validates for Vulkan 1.3");
	Expect(tools.Disassemble(code, &text, SPV_BINARY_TO_TEXT_OPTION_NO_HEADER |
	                                        SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES),
	       "optimized module disassembles");
	return text;
}

size_t Count(const std::string& text, const char* instruction) {
	size_t count = 0;
	for (size_t pos = 0; (pos = text.find(instruction, pos)) != std::string::npos; ++pos) {
		++count;
	}
	return count;
}

// Observable SSBO writes and a deliberately unused descriptor. The unused descriptor remains
// part of the entry point's interface, which the recompiler's binding metadata still describes.
std::string Module(const std::string& body, const std::string& declarations = "",
                   const std::string& decorations = "") {
	return R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %buffer %unused_buffer
OpExecutionMode %main LocalSize 1 1 1
OpName %main "main"
OpName %buffer "buffer"
OpName %unused_buffer "unused_buffer"
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %buffer DescriptorSet 0
OpDecorate %buffer Binding 0
OpDecorate %unused_buffer DescriptorSet 0
OpDecorate %unused_buffer Binding 1
)" + decorations + R"(
%void = OpTypeVoid
%fn = OpTypeFunction %void
%bool = OpTypeBool
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%false = OpConstantFalse %bool
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%seven = OpConstant %uint 7
%unused_constant = OpConstant %uint 999
%semantics = OpConstant %uint 264
%block = OpTypeStruct %uint
%ptr_block = OpTypePointer StorageBuffer %block
%ptr_uint = OpTypePointer StorageBuffer %uint
%ptr_local = OpTypePointer Function %uint
%buffer = OpVariable %ptr_block StorageBuffer
%unused_buffer = OpVariable %ptr_block StorageBuffer
)" + declarations + R"(
%main = OpFunction %void None %fn
%entry = OpLabel
)" + body + R"(
OpReturn
OpFunctionEnd
)";
}

void TestCleanupAndInterface() {
	auto code = Assemble(Module(R"(
%local = OpVariable %ptr_local Function
OpStore %local %seven
%value = OpLoad %uint %local
%dead = OpIMul %uint %seven %seven
%slot = OpAccessChain %ptr_uint %buffer %zero
OpSelectionMerge %merge None
OpBranchConditional %false %dead_block %merge
%dead_block = OpLabel
OpStore %slot %unused_constant
OpBranch %merge
%merge = OpLabel
OpStore %slot %value
)"));
	const auto before = code;
	std::string diagnostic;
	Expect(Recompiler::Spirv::OptimizeProgram(code, diagnostic), "cleanup succeeds");
	Expect(code.size() < before.size(), "redundant code is reduced");
	Expect(diagnostic.empty(), "successful cleanup has no diagnostics");
	const auto text = Disassemble(code);
	Expect(Count(text, "OpIMul") == 0, "dead arithmetic is removed");
	Expect(Count(text, "OpBranchConditional") == 0, "constant dead branch is removed");
	Expect(Count(text, "OpLoad") == 0, "local store-to-load is forwarded");
	Expect(Count(text, "OpStore") == 1, "observable SSBO write remains");
	Expect(text.find("OpConstant %uint 7") != std::string::npos, "stored integer value remains seven");
	Expect(text.find("OpConstant %uint 999") == std::string::npos, "dead constant is removed");
	Expect(text.find("Binding 1") != std::string::npos, "unused descriptor binding remains");
	Expect(text.find("OpEntryPoint GLCompute %main \"main\" %buffer %unused_buffer") != std::string::npos,
	       "entry point interface is preserved");
}

void TestMemoryEffectsAndPreciseFloats() {
	auto code = Assemble(Module(R"(
%slot = OpAccessChain %ptr_uint %buffer %zero
%bits = OpLoad %uint %slot Volatile
%input = OpBitcast %float %bits
%product = OpFMul %float %input %input
%sum = OpFAdd %float %product %input
%result = OpBitcast %uint %sum
OpStore %slot %result Volatile
%unused_atomic_result = OpAtomicIAdd %uint %slot %one %zero %one
OpControlBarrier %two %two %semantics
)", "", "OpDecorate %product NoContraction\nOpDecorate %sum NoContraction\n"));
	std::string diagnostic;
	Expect(Recompiler::Spirv::OptimizeProgram(code, diagnostic), "memory/precise-float optimization succeeds");
	const auto text = Disassemble(code);
	Expect(Count(text, "OpFMul") == 1 && Count(text, "OpFAdd") == 1,
	       "unfused floating-point multiply/add remain");
	Expect(Count(text, "NoContraction") == 2, "NoContraction decorations remain");
	Expect(Count(text, "Volatile") == 2, "volatile load and store remain");
	Expect(Count(text, "OpAtomicIAdd") == 1, "atomic with an unused return remains");
	Expect(Count(text, "OpControlBarrier") == 1, "control barrier remains");
}

void TestSpirv13Bindings() {
	auto text = Module("%slot = OpAccessChain %ptr_uint %buffer %zero\nOpStore %slot %seven\n");
	const std::string entry = "%main \"main\" %buffer %unused_buffer";
	text.replace(text.find(entry), entry.size(), "%main \"main\"");
	// SPIR-V 1.3 lists only input/output variables in OpEntryPoint; descriptors can be unused
	// but must still match the binding metadata/layout returned by CompileProgram.
	auto code = Assemble(text, SPV_ENV_VULKAN_1_1);
	std::string diagnostic;
	Expect(Recompiler::Spirv::OptimizeProgram(code, diagnostic), "SPIR-V 1.3 optimization succeeds");
	const auto optimized = Disassemble(code);
	Expect(code[1] == 0x00010300u, "SPIR-V version is unchanged");
	Expect(optimized.find("Binding 1") != std::string::npos,
	       "unused SPIR-V 1.3 descriptor outside the entry-point interface is preserved");
}

void TestDivisionIsNotReassociated() {
	auto code = Assemble(Module(R"(
%slot = OpAccessChain %ptr_uint %buffer %zero
%bits = OpLoad %uint %slot
%input = OpBitcast %float %bits
%quotient = OpFDiv %float %input %three
%result = OpBitcast %uint %quotient
OpStore %slot %result
)", "%three = OpConstant %float 3\n"));
	std::string diagnostic;
	Expect(Recompiler::Spirv::OptimizeProgram(code, diagnostic), "division optimization succeeds");
	const auto text = Disassemble(code);
	Expect(Count(text, "OpFDiv") == 1 && Count(text, "OpFMul") == 0,
	       "division is not rewritten as a rounded reciprocal multiply");
}

void TestUnusedSpecializationIsPreserved() {
	auto code = Assemble(Module("%slot = OpAccessChain %ptr_uint %buffer %zero\nOpStore %slot %seven\n",
	                            "%unused_spec = OpSpecConstant %uint 17\n",
	                            "OpDecorate %unused_spec SpecId 1\n"));
	std::string diagnostic;
	Expect(Recompiler::Spirv::OptimizeProgram(code, diagnostic), "unused specialization optimization succeeds");
	const auto text = Disassemble(code);
	Expect(Count(text, "OpSpecConstant ") == 1 && text.find("SpecId 1") != std::string::npos,
	       "unused specialization remains in the module's public interface");
}

void TestSpecializationIsNotFrozen() {
	auto code = Assemble(Module(R"(
%slot = OpAccessChain %ptr_uint %buffer %zero
OpSelectionMerge %merge None
OpBranchConditional %enabled %then %merge
%then = OpLabel
OpStore %slot %seven
OpBranch %merge
%merge = OpLabel
)", "%enabled = OpSpecConstantTrue %bool\n", "OpDecorate %enabled SpecId 0\n"));
	std::string diagnostic;
	Expect(Recompiler::Spirv::OptimizeProgram(code, diagnostic), "specialized module optimization succeeds");
	const auto text = Disassemble(code);
	Expect(Count(text, "OpSpecConstantTrue") == 1, "specialization constant remains dynamic");
	Expect(Count(text, "OpBranchConditional") == 1, "specialized branch remains");
}

void TestFailureKeepsOriginal() {
	const auto valid = Assemble(Module("%slot = OpAccessChain %ptr_uint %buffer %zero\nOpStore %slot %seven\n"));
	std::vector<std::vector<uint32_t>> invalid {{}, {1, 2, 3, 4, 5}, valid};
	invalid.back().pop_back();
	for (auto& code : invalid) {
		const auto before = code;
		std::string diagnostic;
		Expect(!Recompiler::Spirv::OptimizeProgram(code, diagnostic), "invalid input is rejected");
		Expect(code == before, "failed optimization keeps every original word");
		Expect(!diagnostic.empty(), "failed optimization reports a diagnostic");
	}
}

void TestDisabledIsIdentical() {
	auto options = Recompiler::GetCodegenOptions();
	const auto saved = options;
	options.spirv_optimize = false;
	Recompiler::SetCodegenOptions(options);
	auto code = Assemble(Module("%dead = OpIMul %uint %seven %seven\n"));
	const auto before = code;
	std::string diagnostic = "stale diagnostic";
	Expect(Recompiler::Spirv::OptimizeProgram(code, diagnostic), "disabled optimization succeeds");
	Expect(code == before, "disabled optimization is byte-identical");
	Expect(diagnostic.empty(), "disabled optimization clears stale diagnostics");
	Recompiler::SetCodegenOptions(saved);
}

} // namespace

int main(int argc, char** argv) {
	if (argc == 2) {
		const bool enabled = std::strcmp(argv[1], "--expect-disabled") != 0;
		Expect(Recompiler::GetCodegenOptions().spirv_optimize == enabled, "environment controls optimization");
	} else {
		auto options = Recompiler::GetCodegenOptions();
		options.spirv_optimize = true;
		Recompiler::SetCodegenOptions(options);
		TestCleanupAndInterface();
		TestMemoryEffectsAndPreciseFloats();
		TestSpirv13Bindings();
		TestDivisionIsNotReassociated();
		TestUnusedSpecializationIsPreserved();
		TestSpecializationIsNotFrozen();
		TestFailureKeepsOriginal();
		TestDisabledIsIdentical();
	}
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::puts("spirv_optimizer: all tests passed");
	return 0;
}
