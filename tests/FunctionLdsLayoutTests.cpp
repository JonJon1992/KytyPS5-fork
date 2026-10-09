#include "graphics/shader/recompiler/ir/passes/FunctionLdsLayout.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

using namespace Libs::Graphics::ShaderRecompiler::IR;
using Libs::Graphics::ShaderType;

void Check(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

struct Fixture {
	Program program;
	Block* block = nullptr;

	Fixture() {
		program.stage = ShaderType::Pixel;
		program.block_storage.push_back(std::make_unique<Block>());
		block = program.block_storage.back().get();
		program.blocks.push_back(block);
	}

	Value Emit(ValueOpcode opcode, std::initializer_list<Value> args = {}, uint64_t flags = 0) {
		return Value(&block->AppendNewInst(opcode, args, flags));
	}

	Value Address(uint32_t shift = 2u) {
		return Emit(ValueOpcode::ShiftLeftLogical32, {Emit(ValueOpcode::LaneId), Value(shift)});
	}

	Value Access(ValueOpcode opcode, uint32_t offset, Value address,
	             ResourceKind kind = ResourceKind::Lds) {
		const MemoryFlags flags {static_cast<uint32_t>(program.memory_info.size()), 0};
		program.memory_info.push_back(MemoryInfo {.kind = kind, .offset = offset});
		uint64_t bits = 0;
		std::memcpy(&bits, &flags, sizeof(flags));
		if (opcode == ValueOpcode::WriteSharedU32 || opcode == ValueOpcode::SharedAtomicIAdd32) {
			return Emit(opcode, {address, Value(7u), Value(true)}, bits);
		}
		return Emit(opcode, {address, Value(true)}, bits);
	}

	Value Load(uint32_t offset = 0u) {
		return Access(ValueOpcode::LoadSharedU32, offset, Address());
	}
};

void Reject(const Fixture& fixture, const char* message) {
	const auto layout = PlanFunctionLdsLayout(fixture.program);
	Check(layout.slots.empty() && layout.dwords == 0u, message);
}

// A planner that treats byte offsets as unbounded splits aliases that the DS
// address mask makes identical. Check aliases and distinct cells together.
void TestAliasesAndWrap() {
	Fixture fixture;
	const auto store = fixture.Access(ValueOpcode::WriteSharedU32, 0u, fixture.Address());
	const auto alias = fixture.Load(65536u);
	const auto below_bound = fixture.Load(32764u);
	const auto wraps = fixture.Load(65532u);
	const auto wraps_alias = fixture.Load(131068u);
	const auto layout = PlanFunctionLdsLayout(fixture.program);
	Check(layout.dwords == 3u && layout.slots.size() == 5u,
	      "16-bit wrapping offsets must retain exactly three distinct cells");
	Check(layout.slots.at(store.TryInstruction()) == layout.slots.at(alias.TryInstruction()),
	      "offsets 0 and 65536 must alias");
	Check(layout.slots.at(wraps.TryInstruction()) == layout.slots.at(wraps_alias.TryInstruction()),
	      "offsets 65532 and 131068 must alias");
	Check(layout.slots.at(store.TryInstruction()) != layout.slots.at(below_bound.TryInstruction()) &&
	          layout.slots.at(store.TryInstruction()) != layout.slots.at(wraps.TryInstruction()) &&
	          layout.slots.at(below_bound.TryInstruction()) != layout.slots.at(wraps.TryInstruction()),
	      "bounds-crossing and wrapping addresses must retain separate cells");
}

void TestStagesAndEmpty() {
	Fixture fixture;
	Reject(fixture, "no LDS accesses must leave storage unchanged");
	fixture.Load();
	for (auto stage: {ShaderType::Compute, ShaderType::Mesh, ShaderType::Vertex,
	                  ShaderType::Local, ShaderType::TessellationControl,
	                  ShaderType::TessellationEvaluation}) {
		fixture.program.stage = stage;
		Reject(fixture, "only pixel LDS may use the compact private layout");
	}
}

void TestOversizedLayout() {
	for (uint32_t count: {8193u, 8191u, 8192u}) {
		Fixture fixture;
		const auto address = fixture.Address();
		for (uint32_t index = 0; index < count; ++index) {
			fixture.Access(ValueOpcode::LoadSharedU32, index * 4u, address);
		}
		if (count == 8191u) {
			Check(PlanFunctionLdsLayout(fixture.program).dwords == 8191u,
			      "a layout smaller than the original LDS allocation must remain eligible");
		} else {
			Reject(fixture, "compaction must shrink the original 8192-dword LDS allocation");
		}
	}
}

void TestUnsupportedAccesses() {
	for (auto opcode: {ValueOpcode::LoadSharedU8, ValueOpcode::LoadSharedU16,
	                   ValueOpcode::LoadSharedU32x2, ValueOpcode::LoadSharedU32x3,
	                   ValueOpcode::LoadSharedU32x4, ValueOpcode::SharedAtomicIAdd32}) {
		Fixture fixture;
		fixture.Load();
		fixture.Access(opcode, 4u, fixture.Address());
		Reject(fixture, "a single incompatible LDS access must reject the entire layout");
	}
	Fixture mixed;
	mixed.Load();
	mixed.Access(ValueOpcode::LoadSharedU16, 0u, Value(0u), ResourceKind::Gds);
	Check(PlanFunctionLdsLayout(mixed.program).dwords == 1u,
	      "GDS storage must not be included in pixel LDS slots");
}

void TestMetadataAndAddresses() {
	{
		Fixture fixture;
		fixture.Load();
		fixture.Emit(ValueOpcode::LoadSharedU32, {fixture.Address(), Value(true)}, 999u);
		Reject(fixture, "invalid memory metadata must reject the entire layout");
	}
	{
		Fixture fixture;
		fixture.Load();
		fixture.Load(65537u);
		Reject(fixture, "unaligned offsets must reject the entire layout");
	}
	for (uint32_t shift: {0u, 1u, 3u}) {
		Fixture fixture;
		fixture.Load();
		fixture.Access(ValueOpcode::LoadSharedU32, 0u, fixture.Address(shift));
		Reject(fixture, "mixed lane scales must reject the entire layout");
	}
	{
		Fixture fixture;
		fixture.Load();
		fixture.Access(ValueOpcode::LoadSharedU32, 0u, Value(0u));
		Reject(fixture, "constant addresses mixed with lane addresses must reject the layout");
	}
	{
		Fixture fixture;
		fixture.Load();
		const auto address = fixture.Emit(ValueOpcode::ShiftLeftLogical32,
		                                 {fixture.Emit(ValueOpcode::LaneId),
		                                  fixture.Emit(ValueOpcode::LaneId)});
		fixture.Access(ValueOpcode::LoadSharedU32, 0u, address);
		Reject(fixture, "dynamic lane shifts must reject the entire layout");
	}
	{
		Fixture fixture;
		fixture.Load();
		const auto address = fixture.Emit(ValueOpcode::ShiftLeftLogical32, {Value(1u), Value(2u)});
		fixture.Access(ValueOpcode::LoadSharedU32, 0u, address);
		Reject(fixture, "a shifted value other than LaneId must reject the layout");
	}
}

} // namespace

int main() {
	try {
		TestAliasesAndWrap();
		TestStagesAndEmpty();
		TestOversizedLayout();
		TestUnsupportedAccesses();
		TestMetadataAndAddresses();
	} catch (const std::exception& error) {
		std::cerr << "function LDS layout test failed: " << error.what() << '\n';
		return 1;
	}
	std::cout << "function LDS layout tests passed\n";
	return 0;
}

namespace Common {
int DbgExitHandler(const char*, int, std::string_view text) {
	throw std::runtime_error(std::string(text));
}
int DbgExitHandler(const char*, int, fmt::text_style, std::string_view text) {
	throw std::runtime_error(std::string(text));
}
int DbgExitIfHandler(const char* expression, const char*, int) {
	throw std::runtime_error(expression);
}
int DbgNotImplementedHandler(const char* expression, const char*, int) {
	throw std::runtime_error(expression);
}
void DbgExit(int) { throw std::runtime_error("typed IR assertion failed"); }
} // namespace Common

// Same focused typed-IR implementation set as ResourceTrackingTests.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
