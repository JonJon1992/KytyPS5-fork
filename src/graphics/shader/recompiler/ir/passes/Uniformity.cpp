#include "graphics/shader/recompiler/ir/passes/Uniformity.h"

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

bool PreservesUniformity(ValueOpcode opcode) {
	// Only deterministic, lane-local operations. New opcodes stay Unknown until reviewed.
	switch (opcode) {
		case ValueOpcode::Identity:
		case ValueOpcode::BitCastU16F16:
		case ValueOpcode::BitCastF16U16:
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::BitCastU64F64:
		case ValueOpcode::BitCastF64U64:
		case ValueOpcode::ConvertU16U32:
		case ValueOpcode::ConvertU32U16:
		case ValueOpcode::ConvertU8U32:
		case ValueOpcode::ConvertU32U8:
		case ValueOpcode::ConvertF32F16:
		case ValueOpcode::ConvertF16F32:
		case ValueOpcode::ConvertS32F32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32S32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::ConvertF32F64:
		case ValueOpcode::ConvertF64F32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeConstructU32x3:
		case ValueOpcode::CompositeConstructU32x4:
		case ValueOpcode::CompositeConstructF32x2:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::CompositeExtractU32x3:
		case ValueOpcode::CompositeExtractU32x4:
		case ValueOpcode::FPAbs32:
		case ValueOpcode::FPNeg32:
		case ValueOpcode::FPSaturate32:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::IAddCarry32:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::IMul64:
		case ValueOpcode::SMulHi:
		case ValueOpcode::UMulHi:
		case ValueOpcode::IAbs32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::BitReverse32:
		case ValueOpcode::BitCount32:
		case ValueOpcode::BitCount64:
		case ValueOpcode::FindUMsb32:
		case ValueOpcode::FindUMsb64:
		case ValueOpcode::FindILsb32:
		case ValueOpcode::SMin32:
		case ValueOpcode::UMin32:
		case ValueOpcode::SMax32:
		case ValueOpcode::UMax32:
		case ValueOpcode::SLessThan32:
		case ValueOpcode::SLessThan64:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThan64:
		case ValueOpcode::IEqual32:
		case ValueOpcode::IEqual64:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::INotEqual64:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::SLessThanEqual64:
		case ValueOpcode::ULessThanEqual64:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::UGreaterThan64:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::UGreaterThanEqual32:
		case ValueOpcode::UGreaterThanEqual64:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdEqual32:
		case ValueOpcode::FPOrdNotEqual32:
		case ValueOpcode::FPOrdLessThan32:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThan32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPCmpClass32:
		case ValueOpcode::FPAdd32:
		case ValueOpcode::FPSub32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPFma32:
		case ValueOpcode::FPMad32:
		case ValueOpcode::FPMin32:
		case ValueOpcode::FPMax32:
		case ValueOpcode::FPRoundEven32:
		case ValueOpcode::FPFloor32:
		case ValueOpcode::FPCeil32:
		case ValueOpcode::FPTrunc32: return true;
		default: return false;
	}
}

} // namespace

Uniformity UniformityAnalysis::Get(Value value, uint32_t depth) {
	if (depth >= 128u) {
		return Uniformity::Unknown;
	}
	if (value.IsImmediate()) {
		const auto type = value.GetType();
		return type == Type::Void || type == Type::ScalarReg || type == Type::VectorReg
		           ? Uniformity::Unknown : Uniformity::Uniform;
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) {
		return Uniformity::Unknown;
	}
	const auto [entry, inserted] = m_values.try_emplace(inst, Uniformity::Unknown);
	// Unknown also marks an in-progress node, so a cycle can never prove itself uniform.
	auto& result = entry->second;
	if (inserted) {
		result = Classify(*inst, depth + 1u);
	}
	return result;
}

Uniformity UniformityAnalysis::Classify(const Inst& inst, uint32_t depth) {
	switch (inst.GetOpcode()) {
		case ValueOpcode::GetUserData:
		case ValueOpcode::GetShaderBase: return Uniformity::Uniform;
		case ValueOpcode::LaneId:
		case ValueOpcode::IsHelperInvocation:
		case ValueOpcode::GetAttribute:
		case ValueOpcode::GetAttributeWithBary:
		case ValueOpcode::GetInterpolationParameter: return Uniformity::Divergent;
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
			if (inst.Arg(1) == inst.Arg(2)) {
				return Get(inst.Arg(1), depth);
			}
			break;
		default: break;
	}
	// In particular, no Phi, memory, clock or subgroup opcode is admitted here.
	if (!PreservesUniformity(inst.GetOpcode())) {
		return Uniformity::Unknown;
	}
	auto result = Uniformity::Uniform;
	for (size_t index = 0; index < inst.NumArgs(); index++) {
		const auto argument = Get(inst.Arg(index), depth);
		if (argument == Uniformity::Unknown) {
			return Uniformity::Unknown;
		}
		if (argument == Uniformity::Divergent) {
			result = Uniformity::Divergent;
		}
	}
	return result;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
