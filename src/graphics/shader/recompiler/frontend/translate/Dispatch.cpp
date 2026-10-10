#include "common/assert.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"
#include "graphics/shader/recompiler/CodegenOptions.h"
#include "graphics/shader/recompiler/BvhCapture.h"

namespace Libs::Graphics::ShaderRecompiler::Frontend {

void Translator::FailMissingTranslation(const Decoder::Instruction& inst) {
	EXIT("opcode %s at pc 0x%08x has no IR translation",
	     Decoder::InstructionToString(inst).c_str(), inst.pc);
}

void Translator::TranslateInstruction(const Decoder::Instruction& inst) {
	current_opcode = inst.opcode;
	current_pc     = inst.pc;
	// Exact shader/PC for wave32 or wave64 (the captured Astro program is wave64).
	// Keep the guest TLAS, its current
	// instance transform and every unsupported BLAS on the existing path.
	if (GetCodegenOptions().astro_hardware_rt && program.shader_hash == BvhCapture::AstroShader &&
	    program.stage == ShaderType::Compute &&
	    (program.wave_size == 32 || program.wave_size == 64) && inst.pc == 0x4a0 &&
	    inst.opcode == Decoder::Opcode::V_RCP_F32) {
		const auto sg = [&](uint32_t r) { return ir.GetScalarReg(IR::ScalarReg(r)); };
		const auto vg = [&](uint32_t r) { return ir.GetVectorReg(IR::VectorReg(r)); };
		const auto desc = ir.Emit(IR::ValueOpcode::CompositeConstructU32x4,{sg(24),sg(25),sg(26),sg(27)});
		const auto origin = ir.Emit(IR::ValueOpcode::CompositeConstructU32x4,{vg(11),vg(14),vg(21),vg(23)});
		const auto direction = ir.Emit(IR::ValueOpcode::CompositeConstructU32x4,{vg(15),vg(17),vg(16),sg(51)});
		const auto exec = ir.GetExec();
		const auto result = ir.Emit(IR::ValueOpcode::AstroTrace,{desc,origin,direction,sg(19),exec});
		const auto used = ir.INotEqual(ir.CompositeExtract(result,3),IR::U32(IR::Value(0u)));
		const auto hit = ir.LogicalAnd(used,ir.INotEqual(ir.CompositeExtract(result,1),IR::U32(IR::Value(~0u))));
		const auto write = [&](uint32_t r,IR::U32 value) {
			ir.SetVectorReg(IR::VectorReg(r),ir.Select(hit,value,vg(r)));
		};
		write(23,ir.CompositeExtract(result,0));write(7,ir.CompositeExtract(result,1));
		write(13,ir.CompositeExtract(result,2));write(4,sg(46));write(5,sg(47));
		// Mixed waves still traverse in software for the remaining lanes. The
		// original 0x9c0 restores s44, including lanes completed by native RT.
		const auto remaining = ir.LogicalAnd(exec,ir.LogicalNot(used));
		const auto empty = ir.LogicalNot(ir.AnyLane(remaining));
		ir.SetScalarReg(IR::ScalarReg(51),ir.Select(empty,IR::U32(IR::Value(~0u)),sg(51)));
		ir.SetScalarReg(IR::ScalarReg(16),ir.Select(empty,sg(50),sg(16)));
		ir.SetExec(remaining);
	}

	switch (inst.opcode) {
		case Decoder::Opcode::UNKNOWN:
		case Decoder::Opcode::COUNT:
			EXIT("decoded opcode has no IR translation at pc 0x%08x", inst.pc);
		case Decoder::Opcode::UNSUPPORTED:
			EXIT("unsupported decoded instruction: %s", Decoder::InstructionToString(inst).c_str());
		default: break;
	}

	switch (inst.family) {
		case Decoder::Family::SOP1:
		case Decoder::Family::SOP2:
		case Decoder::Family::SOPK:
		case Decoder::Family::SOPC:
		case Decoder::Family::SOPP: return EmitScalar(inst);
		case Decoder::Family::VOP1:
		case Decoder::Family::VOP2:
		case Decoder::Family::VOP3:
		case Decoder::Family::VOP3P:
		case Decoder::Family::VOPC: return EmitVector(inst);
		case Decoder::Family::SMEM:
		case Decoder::Family::MUBUF:
		case Decoder::Family::MTBUF:
		case Decoder::Family::FLAT:
		case Decoder::Family::DS:
		case Decoder::Family::MIMG: return EmitMemory(inst);
		case Decoder::Family::VINTRP: return EmitInterpolation(inst);
		case Decoder::Family::EXP: return EXP(inst);
		default: return FailMissingTranslation(inst);
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
