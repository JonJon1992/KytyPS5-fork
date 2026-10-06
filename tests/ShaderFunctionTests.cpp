// Expansion cache checks adapted from Senaxx/KytyPS5 (wolverine),
// commit 6b3fbc83d3bf39c89b3bb1adc68b6e50ce66732f.
#include "graphics/host_gpu/renderer/pipeline/programDiskCache.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderFunctions.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Libs::Graphics;
using namespace Libs::Graphics::ShaderRecompiler::Decoder;

namespace {
void Check(bool ok, const char* message) {
	if (!ok) {
		std::fprintf(stderr, "ShaderFunctionTests: failed: %s\n", message);
		std::abort();
	}
}

constexpr uint32_t Sop1(uint32_t op, uint32_t dst, uint32_t src) {
	return 0xbe800000u | (dst << 16u) | (op << 8u) | src;
}
constexpr uint32_t Vmov(uint32_t value) { return 0x7e000200u | (128u + value); }
constexpr uint32_t End = 0xbf810000u;
constexpr uint32_t Trap = 0xbf920000u | ShaderCallMissTrapCode;

// Execute only this fixture's instructions, checking the selected body and incoming SCC.
struct Execution { uint32_t value = 0; bool scc = false; bool trapped = false; uint64_t return_pc = 0; };
Execution Execute(const std::vector<uint32_t>& code, std::array<uint32_t, 16> regs,
                  bool scc, const ShaderCodeReader& read) {
	Execution result {.scc = scc};
	for (uint32_t pc = 0, steps = 0; pc < code.size() && steps++ < 512;) {
		Instruction inst;
		DecodeInstruction(code, pc, inst);
		const auto value = [&](const Operand& op) {
			return op.kind == OperandKind::Sgpr ? regs.at(op.reg) : op.value;
		};
		const auto next = pc + inst.word_count;
		switch (inst.opcode) {
			case Opcode::S_MOV_B32: regs.at(inst.dst.reg) = value(inst.src0); break;
			case Opcode::V_MOV_B32: result.value = value(inst.src0); break;
			case Opcode::S_LSHL_B32:
				regs.at(inst.dst.reg) = value(inst.src0) << (value(inst.src1) & 31u);
				result.scc = regs.at(inst.dst.reg) != 0; break;
			case Opcode::S_BUFFER_LOAD_DWORDX2: {
				const auto address = uint64_t(regs.at(inst.src0.reg)) |
				                     (uint64_t(regs.at(inst.src0.reg + 1)) << 32u);
				Check(read(address + value(inst.src1) + inst.offset,
				           std::span(regs).subspan(inst.dst.reg, 2)), "runtime table load");
				break;
			}
			case Opcode::S_CMP_EQ_U32: result.scc = value(inst.src0) == value(inst.src1); break;
			case Opcode::S_BRANCH: pc = inst.branch_target / 4u; continue;
			case Opcode::S_CBRANCH_SCC0:
				if (!result.scc) { pc = inst.branch_target / 4u; continue; } break;
			case Opcode::S_CBRANCH_SCC1:
				if (result.scc) { pc = inst.branch_target / 4u; continue; } break;
			case Opcode::S_TRAP: Check(code[pc] == Trap, "reserved miss trap"); result.trapped = true; break;
			case Opcode::S_ENDPGM:
				result.return_pc = uint64_t(regs[8]) | (uint64_t(regs[9]) << 32u);
				return result;
			default: Check(false, "unexpected instruction in expanded fixture");
		}
		pc = next;
	}
	Check(false, "expanded fixture did not terminate");
	return result;
}

ProgramDiskCache::SourceKey DiskKey(const std::vector<uint32_t>& code) {
	ProgramDiskCache::SourceKey key;
	ProgramDiskCache::BuildSourceKey({.stage = 7, .hash = 123, .user_data_count = 4,
	                                 .code_size = 2, .wave_size = 64, .code = code}, key);
	return key;
}
} // namespace

int main() {
	constexpr uint64_t base = 0x20000000u, a = 0x10000000u, b = a + 256u, table = a + 512u;
	std::vector<uint32_t> memory(256, End);
	memory[0] = Vmov(1); memory[1] = Sop1(0x20, 0, 8);
	memory[64] = Vmov(2); memory[65] = Sop1(0x20, 0, 8);
	memory[128] = uint32_t(a); memory[129] = 0;
	memory[130] = uint32_t(b); memory[131] = 0;
	memory[132] = 0; memory[133] = 0;
	bool mapped = true;
	const ShaderCodeReader read = [&](uint64_t address, std::span<uint32_t> words) {
		if (!mapped || address < a || (address & 3u) ||
		    address - a > memory.size() * 4u || words.size_bytes() > memory.size() * 4u - (address - a)) return false;
		std::copy_n(memory.begin() + (address - a) / 4u, words.size(), words.begin());
		return true;
	};
	ShaderFunctionExpander expander;
	std::vector<uint32_t> expanded, reference, dependencies;
	std::string reason, reference_reason;
	const std::vector<uint32_t> direct {Sop1(0x21, 8, 0), End};
	std::array<uint32_t, 16> regs {uint32_t(a), 0, 17};
	const auto expand = [&](const std::vector<uint32_t>& code, uint32_t wave = 64) {
		const bool ok = expander.Expand(code, base, regs, read, expanded, reason, wave);
		const bool reference_ok = InlineShaderFunctions(code, base, regs, read, reference,
		                                               reference_reason, wave, &dependencies);
		Check(ok == reference_ok && expanded == reference && reason == reference_reason,
		      "cached expansion differs from fresh expansion");
		return ok;
	};
	for (const auto wave: {32u, 64u}) {
		Check(expand(direct, wave) && !expanded.empty(), "direct call expansion");
		Check(dependencies == std::vector<uint32_t>({0, 1}), "direct user-data dependencies");
		for (const bool scc: {false, true}) {
			const auto run = Execute(expanded, regs, scc, read);
			Check(run.value == 1 && run.scc == scc && !run.trapped, "direct call semantics");
		}
	}
	const auto original = expanded;
	// Distinct transient snapshots have one guest identity. The guest PC must never become
	// either snapshot's allocator address; reuse of that storage for changed code must refresh.
	const auto owned_first = direct;
	auto owned_second = direct;
	Check(owned_first.data() != owned_second.data(), "owned snapshot fixture shares storage");
	Check(expand(owned_first) && expanded == original, "first owned snapshot changed expansion");
	Check(expand(owned_second) && expanded == original, "second owned snapshot changed identity");
	Check(Execute(expanded, regs, true, read).return_pc == base + 4u,
	      "owned snapshot substituted allocator address for guest return PC");
	owned_second.insert(owned_second.end() - 1, Vmov(4));
	Check(expand(owned_second) && Execute(expanded, regs, true, read).value == 4,
	      "changed owned caller reused stale analysis under same guest identity");
	constexpr uint64_t relocated_base = base + 0x1000u;
	Check(expander.Expand(owned_first, relocated_base, regs, read, expanded, reason, 64) &&
	      Execute(expanded, regs, false, read).return_pc == relocated_base + 4u,
	      "same snapshot under a different guest identity kept stale PCs");
	Check(expand(direct) && expanded == original, "original caller identity did not recover");
	regs[2] = 19;
	Check(expand(direct) && expanded == original, "unrelated register invalidates expansion");
	regs[0] = uint32_t(b);
	Check(expand(direct) && Execute(expanded, regs, true, read).value == 2, "changed direct target");
	regs[0] = uint32_t(a);
	memory[0] = Vmov(3);
	Check(expand(direct) && expanded != original && Execute(expanded, regs, false, read).value == 3,
	      "changed callee reused stale expansion");
	Check(DiskKey(original).digest != DiskKey(expanded).digest, "changed callee disk identity");
	mapped = false;
	Check(!expand(direct) && expanded.empty() && !reason.empty(), "unmapped target accepted");
	mapped = true;
	Check(expand(direct), "failed read did not invalidate when mapping returned");
	regs[0] = uint32_t(a + 1u);
	Check(!expand(direct) && expanded.empty(), "unaligned target accepted");
	Check(direct == std::vector<uint32_t>({Sop1(0x21, 8, 0), End}), "unresolved call changed original code");

	// s_lshl_b32 s7,s6,3; s_buffer_load_dwordx2 s[4:5],s[0:3],s7; swappc s[8:9],s[4:5].
	const std::vector<uint32_t> dynamic {0x8f078306u, 0xf4000000u | (9u << 18u) | (4u << 6u),
	                                    7u << 25u, Sop1(0x21, 8, 4), End};
	regs = {uint32_t(table), 0, 24, 0};
	Check(expand(dynamic), "bounded table expansion");
	Check(dependencies == std::vector<uint32_t>({0, 1, 2, 3}), "table descriptor dependencies");
	for (uint32_t slot = 0; slot < 3; ++slot) {
		regs[6] = slot;
		const auto run = Execute(expanded, regs, false, read);
		Check(run.trapped == (slot == 2) && (run.trapped || run.value == (slot == 0 ? 3u : 2u)),
		      "runtime table selected wrong callee or missed null trap");
		Check(run.trapped || run.scc == (slot != 0), "table call clobbered incoming SCC");
	}
	const auto old_table = expanded;
	regs[6] = 1;
	memory[130] = uint32_t(a + 768u);
	Check(Execute(expanded, regs, true, read).trapped, "target outside captured set did not trap");
	memory[130] = uint32_t(a);
	Check(expand(dynamic) && expanded != old_table, "changed table target reused stale expansion");
	regs[1] = 1u << 31u;
	Check(!expand(dynamic) && expanded.empty(), "swizzled table accepted");
	const std::vector<uint32_t> fetch {Sop1(0x21, 125, 0), End};
	Check(expand(fetch) && expanded.empty(), "embedded fetch call treated as scalar function");
	Check(expand(std::vector<uint32_t> {Vmov(1), End}) && expanded.empty(), "no-call shader expanded");
	std::puts("ShaderFunctionTests: direct/table calls, miss traps, dependencies and invalidation passed");
}
