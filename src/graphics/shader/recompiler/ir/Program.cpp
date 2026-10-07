#include "common/assert.h"
#include "graphics/shader/recompiler/CodegenOptions.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <fmt/format.h>
#include <map>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {

namespace {

[[noreturn]] void Fail(std::string_view message) {
	EXIT("shader IR validation failed: %s", std::string(message).c_str());
	std::abort();
}

bool IsRegisterStatePseudo(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::GetThreadBitScalarRegister:
		case ValueOpcode::SetThreadBitScalarRegister:
		case ValueOpcode::GetScalarMaskTag:
		case ValueOpcode::SetScalarMaskTag:
		case ValueOpcode::GetScalarRegister:
		case ValueOpcode::SetScalarRegister:
		case ValueOpcode::GetVectorRegister:
		case ValueOpcode::SetVectorRegister:
		case ValueOpcode::GetGotoVariable:
		case ValueOpcode::SetGotoVariable:
		case ValueOpcode::GetScc:
		case ValueOpcode::SetScc:
		case ValueOpcode::GetExec:
		case ValueOpcode::SetExec:
		case ValueOpcode::GetExecLo:
		case ValueOpcode::SetExecLo:
		case ValueOpcode::GetExecHi:
		case ValueOpcode::SetExecHi:
		case ValueOpcode::GetVcc:
		case ValueOpcode::SetVcc:
		case ValueOpcode::GetVccLo:
		case ValueOpcode::SetVccLo:
		case ValueOpcode::GetVccHi:
		case ValueOpcode::SetVccHi:
		case ValueOpcode::GetM0:
		case ValueOpcode::SetM0: return true;
		default: return false;
	}
}

bool IsRuntimeRead(ValueOpcode opcode) {
	return opcode == ValueOpcode::ReadConstBuffer ||
	       AddressOpcodeInfoOf(opcode).access == AddressAccess::Read;
}

bool EquivalentValue(const ResourcePlan& program, Value left, Value right,
                     std::vector<std::pair<const Inst*, const Inst*>>& visited) {
	left  = left.Resolve();
	right = right.Resolve();
	if (left == right) {
		return true;
	}
	if (left.IsImmediate() || right.IsImmediate() || left.GetType() != right.GetType()) {
		return false;
	}
	const auto* lhs = left.TryInstruction();
	const auto* rhs = right.TryInstruction();
	if (lhs == nullptr || rhs == nullptr || lhs->GetOpcode() != rhs->GetOpcode() ||
	    lhs->NumArgs() != rhs->NumArgs()) {
		return false;
	}
	if (std::ranges::find(visited, std::pair {lhs, rhs}) != visited.end()) {
		return true;
	}
	visited.emplace_back(lhs, rhs);
	if (IsRuntimeRead(lhs->GetOpcode())) {
		const auto li = lhs->Flags<MemoryFlags>().index;
		const auto ri = rhs->Flags<MemoryFlags>().index;
		if (li >= program.memory_info.size() || ri >= program.memory_info.size() ||
		    program.memory_info[li] != program.memory_info[ri]) {
			return false;
		}
	} else if (lhs->Flags<uint64_t>() != rhs->Flags<uint64_t>()) {
		return false;
	}
	for (size_t index = 0; index < lhs->NumArgs(); index++) {
		if (lhs->GetOpcode() == ValueOpcode::Phi && lhs->PhiBlock(index) != rhs->PhiBlock(index)) {
			return false;
		}
		if (!EquivalentValue(program, lhs->Arg(index), rhs->Arg(index), visited)) {
			return false;
		}
	}
	return true;
}

} // namespace

ResourcePlan::~ResourcePlan() {
	for (auto& inst: value_storage) {
		inst.Invalidate();
	}
}

ResourcePlan& ResourcePlan::operator=(ResourcePlan&& other) noexcept {
	if (this != &other) {
		this->~ResourcePlan();
		new (this) ResourcePlan(std::move(other));
	}
	return *this;
}

Program::~Program() {
	// KYTY_IR_LINEAR_USES: every instruction of both goes now, so nobody's use list needs
	// maintaining (removing each use one by one was quadratic for widely used values).
	const bool drop = GetCodegenOptions().ir_linear_uses;
	// Planning expressions can refer to block values but outlive block storage in the base class.
	for (auto& inst: value_storage) {
		if (drop) {
			inst.DropForDestruction();
		} else {
			inst.Invalidate();
		}
	}
	// Values may cross block boundaries. Detach all arguments before any block starts destroying
	// its instruction storage so reverse-use links always point to live definitions.
	if (drop) {
		// Every owned block, listed or not: an instruction left attached would look for its entry
		// in a dropped list when it is destroyed.
		for (auto& block: block_storage) {
			if (block != nullptr) {
				for (auto& inst: *block) {
					inst.DropForDestruction();
				}
			}
		}
		return;
	}
	for (auto* block: blocks) {
		for (auto& inst: *block) {
			inst.Invalidate();
		}
	}
}

Program& Program::operator=(Program&& other) noexcept {
	if (this != &other) {
		this->~Program();
		new (this) Program(std::move(other));
	}
	return *this;
}

CompiledShaderInfo Program::TakeCompiledInfo() && {
	CompiledShaderInfo result {
	    .stage           = stage,
	    .shader_hash     = shader_hash,
	    .wave_size       = wave_size,
	    .user_data_base  = user_data_base,
	    .user_data_count = user_data_count,
	    .scratch_dwords  = scratch_dwords,
	    .has_address_writes = has_address_writes,
	    .info            = std::move(info),
	    .bindings        = std::move(bindings),
	    .write_ranges    = std::move(write_ranges),
	};
	for (const auto& output: result.info.outputs) {
		if (output.kind == StageOutputKind::Parameter && output.index < 32) {
			result.param_export_mask |= 1u << output.index;
		}
	}
	return result;
}

bool EquivalentValue(const ResourcePlan& program, Value left, Value right) {
	std::vector<std::pair<const Inst*, const Inst*>> visited;
	return EquivalentValue(program, left, right, visited);
}

Value ResolveInvariantPhi(const ResourcePlan& program, Value value) {
	value            = value.Resolve();
	const auto* root = value.TryInstruction();
	if (root == nullptr || root->GetOpcode() != ValueOpcode::Phi) {
		return value;
	}
	Value                           invariant;
	std::vector<Value>              pending {value};
	std::unordered_set<const Inst*> visited;
	while (!pending.empty()) {
		const auto current = pending.back().Resolve();
		pending.pop_back();
		const auto* inst = current.TryInstruction();
		if (inst != nullptr && inst->GetOpcode() == ValueOpcode::Phi) {
			if (!visited.insert(inst).second) {
				continue;
			}
			for (size_t index = 0; index < inst->NumArgs(); index++) {
				pending.push_back(inst->Arg(index));
			}
			continue;
		}
		if (invariant.IsEmpty()) {
			invariant = current;
		} else if (!EquivalentValue(program, invariant, current)) {
			return {};
		}
	}
	return invariant;
}

bool HasShaderMemoryWrites(const Program& program, bool include_image_writes) {
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto op     = inst.GetOpcode();
			const auto buffer = BufferAccessOf(op);
			const auto image  = ImageOpcodeInfoOf(op).access;
			if (buffer == BufferAccess::Write || buffer == BufferAccess::Atomic ||
			    (include_image_writes &&
			     (image == ImageAccess::Write || image == ImageAccess::Atomic)) ||
			    AddressOpcodeInfoOf(op).access == AddressAccess::Write) {
				return true;
			}
		}
	}
	return false;
}

void ValidateProgram(const Program& program, bool require_ssa) {
	if (program.blocks.size() != program.block_info.size() ||
	    program.blocks.size() != program.block_storage.size()) {
		return Fail("value IR block storage is inconsistent");
	}
	// Per instruction: its position in its block and the index of its first argument in `covered`.
	struct InstructionSlot {
		size_t position  = 0;
		size_t args_base = 0;
	};
	std::unordered_map<const Block*, size_t>        block_indices;
	std::unordered_map<uint32_t, const Block*>      blocks_by_id;
	std::unordered_map<const Inst*, InstructionSlot> instruction_positions;
	size_t                                          instruction_count = 0;
	for (const auto* block: program.blocks) {
		instruction_count += block != nullptr ? block->Instructions().size() : 0u;
	}
	block_indices.reserve(program.blocks.size());
	blocks_by_id.reserve(program.blocks.size());
	instruction_positions.reserve(instruction_count);
	size_t argument_count = 0;
	for (size_t block_index = 0; block_index < program.blocks.size(); block_index++) {
		const auto* block = program.blocks[block_index];
		if (block == nullptr || program.block_storage[block_index] == nullptr ||
		    block != program.block_storage[block_index].get()) {
			return Fail("value IR block pointer is inconsistent");
		}
		if (!block_indices.emplace(block, block_index).second) {
			return Fail("value IR block pointer is duplicated");
		}
		if (program.block_info[block_index].id == UINT32_MAX) {
			return Fail("value IR block uses the reserved exit id");
		}
		if (!blocks_by_id.emplace(program.block_info[block_index].id, block).second) {
			return Fail("value IR block id is duplicated");
		}
		size_t position = 0;
		for (const auto& inst: *block) {
			if (!instruction_positions.emplace(&inst, InstructionSlot {position++, argument_count})
			         .second) {
				return Fail("value IR instruction is duplicated");
			}
			argument_count += inst.NumArgs();
		}
	}
	// covered[slot]: the definition the argument refers to lists it among its uses. One pass over
	// the use lists replaces a search of the definition's whole use list for every argument.
	std::vector<uint8_t> covered(argument_count, 0u);
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			for (const auto& use: inst.Uses()) {
				const auto user = instruction_positions.find(use.user);
				if (user != instruction_positions.end() && use.operand < use.user->NumArgs() &&
				    use.user->Arg(use.operand).TryInstruction() == &inst) {
					auto& slot = covered[user->second.args_base + use.operand];
					if (slot != 0u) {
						return Fail("value IR reverse use is duplicated");
					}
					slot = 1u;
				}
			}
		}
	}
	if (!program.blocks.empty() && !program.blocks.front()->ImmPredecessors().empty()) {
		return Fail("value IR entry block has a predecessor");
	}

	for (size_t block_index = 0; block_index < program.blocks.size(); block_index++) {
		const auto*                      block = program.blocks[block_index];
		std::unordered_set<const Block*> predecessors;
		for (const auto* predecessor: block->ImmPredecessors()) {
			if (predecessor == nullptr || !block_indices.contains(predecessor)) {
				return Fail("value IR block has a foreign predecessor");
			}
			if (!predecessors.insert(predecessor).second) {
				return Fail("value IR block predecessor is duplicated");
			}
			if (std::ranges::find(predecessor->ImmSuccessors(), block) ==
			    predecessor->ImmSuccessors().end()) {
				return Fail("value IR predecessor edge is not reciprocal");
			}
		}

		std::unordered_set<const Block*> successors;
		for (const auto* successor: block->ImmSuccessors()) {
			if (successor == nullptr || !block_indices.contains(successor)) {
				return Fail("value IR block has a foreign successor");
			}
			if (!successors.insert(successor).second) {
				return Fail("value IR block successor is duplicated");
			}
			if (std::ranges::find(successor->ImmPredecessors(), block) ==
			    successor->ImmPredecessors().end()) {
				return Fail("value IR successor edge is not reciprocal");
			}
		}

		std::unordered_set<const Block*> expected_successors;
		const auto                       add_target = [&](uint32_t id) {
			const auto found = blocks_by_id.find(id);
			if (found == blocks_by_id.end()) {
				return false;
			}
			expected_successors.insert(found->second);
			return true;
		};
		const auto& terminator             = program.block_info[block_index].terminator;
		const auto  validate_control_value = [&](Value value, Type type) {
			if (value.IsEmpty() || value.GetType() != type) {
				return false;
			}
			const auto* definition = value.TryInstruction();
			return definition == nullptr || instruction_positions.contains(definition);
		};
		switch (terminator.kind) {
			case CFG::TerminatorKind::Branch:
				if (!add_target(terminator.true_block)) {
					return Fail("value IR branch target is missing");
				}
				break;
			case CFG::TerminatorKind::ConditionalBranch:
				if (!add_target(terminator.true_block) || !add_target(terminator.false_block)) {
					return Fail("value IR conditional branch target is missing");
				}
				if (!validate_control_value(program.block_info[block_index].condition, Type::U1)) {
					return Fail("value IR conditional branch condition is invalid");
				}
				break;
			case CFG::TerminatorKind::IndirectBranch: {
				if (!validate_control_value(program.block_info[block_index].indirect_target,
				                            Type::U32)) {
					return Fail("value IR indirect branch selector is invalid");
				}
				std::unordered_set<uint32_t> indirect_targets;
				for (const auto target: terminator.indirect_targets) {
					if (!indirect_targets.insert(target).second) {
						return Fail("value IR indirect branch target is duplicated");
					}
					if (!add_target(target)) {
						return Fail("value IR indirect branch target is missing");
					}
				}
				if (terminator.indirect_selector_values.size() !=
				    terminator.indirect_selector_targets.size()) {
					return Fail("value IR indirect selector table is inconsistent");
				}
				for (const auto target: terminator.indirect_selector_targets) {
					const auto found = blocks_by_id.find(target);
					if (found == blocks_by_id.end() ||
					    !expected_successors.contains(found->second)) {
						return Fail("value IR indirect selector target is not a CFG successor");
					}
				}
				break;
			}
			case CFG::TerminatorKind::Return:
			case CFG::TerminatorKind::Unsupported: break;
		}
		if ((terminator.merge_block != UINT32_MAX &&
		     !blocks_by_id.contains(terminator.merge_block)) ||
		    (terminator.continue_block != UINT32_MAX &&
		     !blocks_by_id.contains(terminator.continue_block))) {
			return Fail("value IR structured control target is missing");
		}
		if (successors != expected_successors) {
			return Fail("value IR terminator and successor graph disagree");
		}

		bool saw_non_phi = false;
		for (const auto& inst: *block) {
			if (inst.Parent() != block) {
				return Fail("value IR instruction has the wrong parent block");
			}
			if (inst.GetOpcode() == ValueOpcode::Phi) {
				if (saw_non_phi) {
					return Fail("value IR Phi appears after a non-Phi instruction");
				}
				if (inst.NumPhiBlocks() != inst.NumArgs()) {
					return Fail("value IR Phi parent table is inconsistent");
				}
				if (inst.NumArgs() == 0) {
					return Fail("value IR Phi has no incoming values");
				}
				std::unordered_set<const Block*> incoming_blocks;
				for (size_t arg_index = 0; arg_index < inst.NumArgs(); arg_index++) {
					const auto* predecessor = inst.PhiBlock(arg_index);
					if (predecessor == nullptr || !block_indices.contains(predecessor) ||
					    !predecessors.contains(predecessor)) {
						return Fail("value IR Phi has a foreign or non-predecessor parent");
					}
					if (!incoming_blocks.insert(predecessor).second) {
						return Fail("value IR Phi parent is duplicated");
					}
					if (inst.Arg(arg_index).GetType() != inst.GetType()) {
						return Fail("value IR Phi incoming type does not match its result");
					}
				}
				if (incoming_blocks != predecessors) {
					return Fail("value IR Phi does not cover every predecessor");
				}
			} else {
				saw_non_phi = true;
			}
			if (require_ssa && IsRegisterStatePseudo(inst.GetOpcode())) {
				return Fail(fmt::format("register-state pseudo {} survived SSA rewrite",
				                        ValueOpcodeName(inst.GetOpcode())));
			}
			if (inst.GetOpcode() != ValueOpcode::Phi && inst.GetOpcode() != ValueOpcode::Identity &&
			    inst.GetType() == Type::Opaque) {
				return Fail(fmt::format("untyped opcode {} survived translation",
				                        ValueOpcodeName(inst.GetOpcode())));
			}
			const bool fixed_signature =
			    inst.GetOpcode() != ValueOpcode::Phi && inst.GetOpcode() != ValueOpcode::Identity;
			if (fixed_signature && inst.NumArgs() != NumArgsOf(inst.GetOpcode())) {
				return Fail(fmt::format("{} has {} arguments, expected {}",
				                        ValueOpcodeName(inst.GetOpcode()), inst.NumArgs(),
				                        NumArgsOf(inst.GetOpcode())));
			}
			if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer) {
				const auto memory_index = inst.Flags<MemoryFlags>().index;
				if (memory_index >= program.memory_info.size()) {
					return Fail(fmt::format("{} has an invalid memory-info index",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
				const auto& memory = program.memory_info[memory_index];
				if (memory.kind != ResourceKind::ScalarBuffer &&
				    memory.kind != ResourceKind::IndirectBuffer) {
					return Fail(fmt::format("{} has an invalid scalar-memory resource kind",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
				const bool valid_group_width =
				    memory.component_count == 1u || memory.component_count == 2u ||
				    memory.component_count == 4u || memory.component_count == 8u ||
				    memory.component_count == 16u;
				if (memory.data_bits != 32u || memory.data_dwords != 1u || !valid_group_width ||
				    memory.component_index >= memory.component_count) {
					return Fail(fmt::format("{} has inconsistent scalar-memory metadata",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
			}
			const auto address_info = AddressOpcodeInfoOf(inst.GetOpcode());
			if (address_info.access != AddressAccess::None) {
				const auto memory_index = inst.Flags<MemoryFlags>().index;
				if (memory_index >= program.memory_info.size()) {
					return Fail(fmt::format("{} has an invalid memory-info index",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
				const auto& memory = program.memory_info[memory_index];
				if (!IsAddressResourceKind(memory.kind) ||
				    (memory.kind == ResourceKind::ScalarAddress &&
				     inst.GetOpcode() != ValueOpcode::LoadAddressU32)) {
					return Fail(fmt::format("{} has an invalid address resource kind",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
				const bool scalar_address = memory.kind == ResourceKind::ScalarAddress;
				const bool valid_group_width =
				    scalar_address
				        ? memory.component_count == 1u || memory.component_count == 2u ||
				              memory.component_count == 4u || memory.component_count == 8u ||
				              memory.component_count == 16u
				        : memory.component_count >= 1u && memory.component_count <= 4u;
				if (memory.data_bits != address_info.data_bits ||
				    memory.data_dwords != address_info.data_dwords ||
				    (address_info.data_dwords > 1u &&
				     (memory.kind != ResourceKind::Global || memory.component_index != 0u)) ||
				    !valid_group_width || memory.component_index >= memory.component_count ||
				    memory.sampler != 0u) {
					return Fail(fmt::format("{} has inconsistent address-memory metadata",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
			}
			const auto buffer_components = BufferComponentCount(inst.GetOpcode());
			if (buffer_components != 0u) {
				const auto memory_index = inst.Flags<MemoryFlags>().index;
				if (memory_index >= program.memory_info.size()) {
					return Fail(fmt::format("{} has an invalid memory-info index",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
				const auto& memory = program.memory_info[memory_index];
				const bool  vector_buffer = memory.kind == ResourceKind::Buffer ||
				                            memory.kind == ResourceKind::IndirectBuffer;
				if (!vector_buffer && memory.kind != ResourceKind::ScalarBuffer) {
					return Fail(fmt::format("{} has a non-buffer resource kind",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
				if (memory.kind == ResourceKind::IndirectBuffer &&
				    !memory.SupportsIndirectBufferLoad(inst.GetOpcode())) {
					return Fail("indirect buffer requires a raw DWORD x2/x3/x4 load");
				}
				if (buffer_components > 1u &&
				    (!vector_buffer || memory.data_bits != 32u ||
				     memory.data_dwords != buffer_components || memory.component_index != 0u)) {
					return Fail(fmt::format("{} has inconsistent native-wide metadata",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
				if (buffer_components == 1u &&
				    (inst.GetOpcode() == ValueOpcode::LoadBufferU32 ||
				     inst.GetOpcode() == ValueOpcode::StoreBufferU32) &&
				    memory.data_dwords != 1u) {
					return Fail(fmt::format("{} retains scalar-sibling width metadata",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
			}
			const auto shared_components = SharedComponentCount(inst.GetOpcode());
			if (shared_components != 0u) {
				const auto memory_index = inst.Flags<MemoryFlags>().index;
				if (memory_index >= program.memory_info.size()) {
					return Fail(fmt::format("{} has an invalid memory-info index",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
				const auto& memory = program.memory_info[memory_index];
				if ((memory.kind != ResourceKind::Lds && memory.kind != ResourceKind::Gds) ||
				    memory.resource != 0u || memory.sampler != 0u || memory.component_count == 0u ||
				    memory.component_index >= memory.component_count) {
					return Fail(fmt::format("{} has invalid shared-memory metadata",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
				uint32_t expected_bits = 32u;
				if (inst.GetOpcode() == ValueOpcode::LoadSharedU8 ||
				    inst.GetOpcode() == ValueOpcode::WriteSharedU8) {
					expected_bits = 8u;
				} else if (inst.GetOpcode() == ValueOpcode::LoadSharedU16 ||
				           inst.GetOpcode() == ValueOpcode::WriteSharedU16) {
					expected_bits = 16u;
				}
				if (memory.data_bits != expected_bits || memory.data_dwords != shared_components ||
				    (shared_components > 1u && memory.component_index != 0u)) {
					return Fail(fmt::format("{} has inconsistent shared-memory width",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
			}
			const auto image_info = ImageOpcodeInfoOf(inst.GetOpcode());
			if (image_info.access != ImageAccess::None) {
				const auto memory_index = inst.Flags<MemoryFlags>().index;
				if (memory_index >= program.memory_info.size()) {
					return Fail(fmt::format("{} has an invalid memory-info index",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
				const auto& memory = program.memory_info[memory_index];
				if (memory.kind != ResourceKind::Image ||
				    image_info.resource_class == ImageResourceClass::None) {
					return Fail(fmt::format("{} has invalid image-memory metadata",
					                        ValueOpcodeName(inst.GetOpcode())));
				}
			}
			if (inst.GetOpcode() == ValueOpcode::SetAttribute &&
			    inst.Flags<ExportFlags>().index >= program.export_info.size()) {
				return Fail("SetAttribute has an invalid export-info index");
			}
			uint32_t composite_components = 0u;
			switch (inst.GetOpcode()) {
				case ValueOpcode::CompositeExtractU64: composite_components = 2u; break;
				case ValueOpcode::CompositeExtractU32x2: composite_components = 2u; break;
				case ValueOpcode::CompositeExtractU32x3: composite_components = 3u; break;
				case ValueOpcode::CompositeExtractU32x4: composite_components = 4u; break;
				default: break;
			}
			if (composite_components != 0u &&
			    (!inst.Arg(1).IsImmediate() || inst.Arg(1).GetType() != Type::U32 ||
			     inst.Arg(1).U32() >= composite_components)) {
				return Fail(fmt::format("{} has an invalid component index",
				                        ValueOpcodeName(inst.GetOpcode())));
			}
			const auto args_base = instruction_positions.at(&inst).args_base;
			for (size_t arg_index = 0; arg_index < inst.NumArgs(); arg_index++) {
				const auto arg = inst.Arg(arg_index);
				if (arg.IsEmpty()) {
					return Fail(
					    fmt::format("{} has an empty argument", ValueOpcodeName(inst.GetOpcode())));
				}
				if (fixed_signature && arg.GetType() != ArgTypeOf(inst.GetOpcode(), arg_index)) {
					return Fail(fmt::format("{} argument {} has type {}, expected {}",
					                        ValueOpcodeName(inst.GetOpcode()), arg_index,
					                        TypeName(arg.GetType()),
					                        TypeName(ArgTypeOf(inst.GetOpcode(), arg_index))));
				}
				if (const auto* definition = arg.TryInstruction();
				    definition != nullptr && !instruction_positions.contains(definition)) {
					return Fail("value IR argument has a foreign definition");
				}
				if (const auto* definition = arg.TryInstruction(); definition != nullptr) {
					if (covered[args_base + arg_index] == 0u) {
						return Fail(fmt::format("{} argument {} is absent from {} reverse uses",
						                        ValueOpcodeName(inst.GetOpcode()), arg_index,
						                        ValueOpcodeName(definition->GetOpcode())));
					}
				}
			}
		}
	}

	if (program.blocks.empty()) {
		return;
	}

	std::vector<bool>   reachable(program.blocks.size(), false);
	std::vector<size_t> pending {0u};
	while (!pending.empty()) {
		const auto block_index = pending.back();
		pending.pop_back();
		if (reachable[block_index]) {
			continue;
		}
		reachable[block_index] = true;
		for (const auto* successor: program.blocks[block_index]->ImmSuccessors()) {
			pending.push_back(block_indices.at(successor));
		}
	}
	if (!std::ranges::all_of(reachable, [](bool value) { return value; })) {
		return Fail("value IR contains an unreachable block");
	}

	// Immediate dominators (Cooper, Harvey and Kennedy) instead of a fixpoint over block-sized
	// sets. Every block is reachable from the entry here, so the dominance relation is the same.
	const auto          block_count = program.blocks.size();
	std::vector<size_t> postorder;
	postorder.reserve(block_count);
	{
		std::vector<uint8_t>                   visited(block_count, 0u);
		std::vector<std::pair<size_t, size_t>> stack {{0u, 0u}};
		visited[0] = 1u;
		while (!stack.empty()) {
			auto& [block_index, next] = stack.back();
			const auto successors     = program.blocks[block_index]->ImmSuccessors();
			if (next < successors.size()) {
				const auto successor = block_indices.at(successors[next++]);
				if (visited[successor] == 0u) {
					visited[successor] = 1u;
					stack.push_back({successor, 0u});
				}
				continue;
			}
			postorder.push_back(block_index);
			stack.pop_back();
		}
	}
	constexpr auto      NoBlock = static_cast<size_t>(-1);
	std::vector<size_t> order(block_count, 0u);
	for (size_t index = 0; index < postorder.size(); index++) {
		order[postorder[index]] = index;
	}
	std::vector<size_t> idom(block_count, NoBlock);
	idom[0]              = 0;
	const auto intersect = [&](size_t a, size_t b) {
		while (a != b) {
			while (order[a] < order[b]) {
				a = idom[a];
			}
			while (order[b] < order[a]) {
				b = idom[b];
			}
		}
		return a;
	};
	for (bool changed = true; changed;) {
		changed = false;
		for (auto it = postorder.rbegin(); it != postorder.rend(); ++it) {
			const auto block_index = *it;
			if (block_index == 0) {
				continue;
			}
			size_t candidate = NoBlock;
			for (const auto* predecessor: program.blocks[block_index]->ImmPredecessors()) {
				const auto predecessor_index = block_indices.at(predecessor);
				if (idom[predecessor_index] != NoBlock) {
					candidate = candidate == NoBlock ? predecessor_index
					                                 : intersect(predecessor_index, candidate);
				}
			}
			if (idom[block_index] != candidate) {
				idom[block_index] = candidate;
				changed           = true;
			}
		}
	}
	// Entry/exit numbers of a dominator-tree walk: a dominates b when b's interval nests in a's.
	std::vector<std::vector<size_t>> children(block_count);
	for (size_t block_index = 1; block_index < block_count; block_index++) {
		children[idom[block_index]].push_back(block_index);
	}
	std::vector<uint32_t> enter(block_count, 0u);
	std::vector<uint32_t> leave(block_count, 0u);
	{
		uint32_t                               clock = 0;
		std::vector<std::pair<size_t, size_t>> stack {{0u, 0u}};
		enter[0] = clock++;
		while (!stack.empty()) {
			auto& [block_index, next] = stack.back();
			if (next < children[block_index].size()) {
				const auto child = children[block_index][next++];
				enter[child]     = clock++;
				stack.push_back({child, 0u});
				continue;
			}
			leave[block_index] = clock++;
			stack.pop_back();
		}
	}

	const auto dominates = [&](const Block* definition, const Block* use) {
		const auto a = block_indices.at(definition);
		const auto b = block_indices.at(use);
		return enter[a] <= enter[b] && leave[b] <= leave[a];
	};
	const auto control_dominates = [&](Value value, const Block* use) {
		const auto* definition = value.TryInstruction();
		return definition == nullptr || definition->Parent() == use ||
		       dominates(definition->Parent(), use);
	};

	for (size_t block_index = 0; block_index < program.blocks.size(); block_index++) {
		const auto* block = program.blocks[block_index];
		for (const auto& inst: *block) {
			for (size_t arg_index = 0; arg_index < inst.NumArgs(); arg_index++) {
				const auto* definition = inst.Arg(arg_index).TryInstruction();
				if (definition == nullptr) {
					continue;
				}
				if (inst.GetOpcode() == ValueOpcode::Phi) {
					const auto* predecessor = inst.PhiBlock(arg_index);
					if (definition->Parent() != predecessor &&
					    !dominates(definition->Parent(), predecessor)) {
						return Fail("value IR Phi incoming definition does not dominate its edge");
					}
				} else if (definition->Parent() == block) {
					if (instruction_positions.at(definition).position >=
					    instruction_positions.at(&inst).position) {
						return Fail("value IR instruction uses a same-block definition before it");
					}
				} else if (!dominates(definition->Parent(), block)) {
					return Fail("value IR instruction definition does not dominate its use");
				}
			}
		}
		const auto& info = program.block_info[block_index];
		if (!info.condition.IsEmpty() && !control_dominates(info.condition, block)) {
			return Fail("value IR branch condition definition does not dominate its use");
		}
		if (!info.indirect_target.IsEmpty() && !control_dominates(info.indirect_target, block)) {
			return Fail("value IR indirect selector definition does not dominate its use");
		}
	}
}

void ResolveControlFlowIdentities(Program& program) {
	for (auto& info: program.block_info) {
		info.condition       = info.condition.Resolve();
		info.indirect_target = info.indirect_target.Resolve();
	}
}

std::string ProgramToString(const Program& program) {
	std::map<const Inst*, size_t> ids;
	size_t                        next_id = 1;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			ids.emplace(&inst, next_id++);
		}
	}
	const auto value_string = [&](Value value) {
		if (value.IsEmpty()) {
			return std::string("<null>");
		}
		if (const auto* inst = value.TryInstruction(); inst != nullptr) {
			return fmt::format("%{}", ids.at(inst));
		}
		switch (value.GetType()) {
			case Type::ScalarReg: return fmt::format("s{}", RegIndex(value.ScalarRegister()));
			case Type::VectorReg: return fmt::format("v{}", RegIndex(value.VectorRegister()));
			case Type::U1: return std::string(value.U1() ? "true" : "false");
			case Type::U8: return fmt::format("{}u8", value.U8());
			case Type::U16: return fmt::format("{}u16", value.U16());
			case Type::U32: return fmt::format("0x{:08x}", value.U32());
			case Type::U64: return fmt::format("0x{:016x}", value.U64());
			case Type::F16: return fmt::format("f16(0x{:04x})", value.F16Bits());
			case Type::F32: return fmt::format("{}f", value.F32Value());
			default: return fmt::format("<{}>", TypeName(value.GetType()));
		}
	};

	std::string text;
	for (size_t block_index = 0; block_index < program.blocks.size(); block_index++) {
		text += fmt::format("Block ${} pc=0x{:08x}..0x{:08x}\n", block_index,
		                    program.block_info[block_index].start_pc,
		                    program.block_info[block_index].end_pc);
		for (const auto& inst: *program.blocks[block_index]) {
			const auto type = inst.GetType();
			if (type != Type::Void) {
				text +=
				    fmt::format("  %{:<5} = {}", ids.at(&inst), ValueOpcodeName(inst.GetOpcode()));
			} else {
				text += fmt::format("          {}", ValueOpcodeName(inst.GetOpcode()));
			}
			for (size_t index = 0; index < inst.NumArgs(); index++) {
				text += index == 0 ? " " : ", ";
				if (inst.GetOpcode() == ValueOpcode::Phi) {
					const auto predecessor =
					    std::ranges::find(program.blocks, inst.PhiBlock(index));
					text += fmt::format("[{}, ${}]", value_string(inst.Arg(index)),
					                    std::distance(program.blocks.begin(), predecessor));
				} else {
					text += value_string(inst.Arg(index));
				}
			}
			text +=
			    fmt::format(" ({}; uses={})\n", TypeName(Value(const_cast<Inst*>(&inst)).GetType()),
			                inst.UseCount());
		}
	}
	return text;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
