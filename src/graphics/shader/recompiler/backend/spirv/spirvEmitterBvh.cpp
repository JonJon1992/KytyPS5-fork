// Adapted from Senaxx/KytyPS5 Wolverine, commit 6b3fbc83d3bf39c89b3bb1adc68b6e50ce66732f (GPL-2.0).
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/shader/recompiler/BvhCapture.h"
#include "graphics/shader/recompiler/CodegenOptions.h"
#include "graphics/shader/recompiler/AstroNativeBinding.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

using Vec3 = std::array<uint32_t, 3>;

uint32_t Extract(EmitterState& s, uint32_t type, uint32_t vector, uint32_t index) {
	const auto result = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpCompositeExtract, type, result, vector, index);
	return result;
}

uint32_t Vector(EmitterState& s, uint32_t type, std::span<const uint32_t> components) {
	const auto result = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpCompositeConstruct, type, result, components);
	return result;
}

uint32_t NodeWord(EmitterState& s, uint32_t block, uint32_t word) {
	const auto offset = Binary(s, spv::OpShiftLeftLogical, TypeU32(s), word, ConstantU32(s, 2));
	const auto address = Binary(s, spv::OpIAdd, TypeScalarU64(s), block,
	                            Unary(s, spv::OpUConvert, TypeScalarU64(s), offset));
	const auto pointer = Unary(s, spv::OpConvertUToPtr, TypePhysicalU32Pointer(s), address);
	const auto result = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpLoad, TypeU32(s), result, pointer, spv::MemoryAccessAlignedMask, 4u);
	return result;
}

bool CaptureApplies(const EmitterState& s) {
	return s.program.stage == ShaderType::Compute && BvhCaptureApplies(s.program.shader_hash);
}

#include "spirvEmitterAstroCapture.inc"

// Records executed calls under guest EXEC, including rejected inputs. Node
// bytes are loaded only after the original bounds/mapping guards succeeded.
void CaptureNode(EmitterState& s, std::span<const uint32_t> args, uint32_t pc, uint32_t invocation,
                 uint32_t first, uint32_t second, uint32_t full, uint32_t status, uint32_t result,
                 uint32_t context) {
	namespace C = BvhCapture;
	const auto u = TypeU32(s), b = TypeBool(s);
	const auto resident = Binary(s, spv::OpIEqual, b, status, ConstantU32(s, C::Resident));
	constexpr auto base = uint32_t(BufferCache::BDA_WRITES_FAULT_BUFFER_SIZE / 4);
	const auto pointer = [&](uint32_t index) {
		const auto p = s.builder.AllocateId();
		s.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(s), p,
		    s.fault_buffer_variable, ConstantU32(s, 0), index);
		return p;
	};
	const auto field = [&](uint32_t index) { return pointer(ConstantU32(s, base + index)); };
	const auto load = [&](uint32_t p, bool atomic) {
		const auto value = s.builder.AllocateId();
		if (atomic) s.builder.AddFunction(spv::OpAtomicLoad, u, value, p,
		    ConstantU32(s, spv::ScopeDevice), ConstantU32(s, spv::MemorySemanticsMaskNone));
		else s.builder.AddFunction(spv::OpLoad, u, value, p);
		return value;
	};
	// When capture is disabled, avoid the counter entirely. Invocation sampling
	// happens before its CAS loop so a large dispatch cannot contend on one word.
	(void)EmitValueOrDefaultIfCondition(s,
	    Binary(s, spv::OpIEqual, b, load(field(C::Enabled), true), ConstantU32(s, 1)),
	    u, ConstantU32(s, 0), [&] {
		const auto x = Extract(s, u, invocation, 0);
		const auto y = Binary(s, spv::OpIMul, u, Extract(s, u, invocation, 1), ConstantU32(s, 0x9e3779b9));
		const auto z = Binary(s, spv::OpIMul, u, Extract(s, u, invocation, 2), ConstantU32(s, 0x85ebca6b));
		const auto hash = Binary(s, spv::OpBitwiseXor, u, x, Binary(s, spv::OpBitwiseXor, u, y, z));
		const auto sampled = Binary(s, spv::OpIEqual, b,
		    Binary(s, spv::OpBitwiseAnd, u, hash, load(field(C::SampleMask), false)), ConstantU32(s, 0));
		const auto selected_pc = load(field(C::PcFilter), false);
		const auto pc_matches = Binary(s, spv::OpLogicalOr, b,
		    Binary(s, spv::OpIEqual, b, selected_pc, ConstantU32(s, 0)),
		    Binary(s, spv::OpIEqual, b, selected_pc, pc));
		return EmitValueOrDefaultIfCondition(s,
		    Binary(s, spv::OpLogicalAnd, b, sampled, pc_matches), u, ConstantU32(s, 0), [&] {
			const auto capacity = load(field(C::Capacity), false);
			const auto count_ptr = field(C::Count);
			const auto observed = load(count_ptr, true);
			auto active = Binary(s, spv::OpULessThanEqual, b, capacity, ConstantU32(s, C::MaxRecords));
			active = Binary(s, spv::OpLogicalAnd, b, active,
			    Binary(s, spv::OpULessThan, b, observed, capacity));
			(void)EmitValueOrDefaultIfCondition(s, active, u, ConstantU32(s, 0), [&] {
				// Saturating CAS reservation: no counter wrap, duplicate slots or stores
				// beyond capacity even when thousands of invocations arrive together.
				const auto pre = s.current_label;
				const auto header = s.builder.AllocateId(), attempt = s.builder.AllocateId();
				const auto next = s.builder.AllocateId(), merge = s.builder.AllocateId();
				const auto expected = s.builder.AllocateId(), old = s.builder.AllocateId();
				s.builder.AddFunction(spv::OpBranch, header);
				EmitLabel(s, header);
				s.builder.AddFunction(spv::OpPhi, u, expected, observed, pre, old, next);
				const auto has_space = Binary(s, spv::OpULessThan, b, expected, capacity);
				s.builder.AddFunction(spv::OpLoopMerge, merge, next, spv::LoopControlMaskNone);
				s.builder.AddFunction(spv::OpBranchConditional, has_space, attempt, merge);
				EmitLabel(s, attempt);
				s.builder.AddFunction(spv::OpAtomicCompareExchange, u, old, count_ptr,
				    ConstantU32(s, spv::ScopeDevice), ConstantU32(s, spv::MemorySemanticsMaskNone),
				    ConstantU32(s, spv::MemorySemanticsMaskNone),
				    Binary(s, spv::OpIAdd, u, expected, ConstantU32(s, 1)), expected);
				const auto claimed = Binary(s, spv::OpIEqual, b, old, expected);
				s.builder.AddFunction(spv::OpBranchConditional, claimed, merge, next);
				EmitLabel(s, next);
				s.builder.AddFunction(spv::OpBranch, header);
				EmitLabel(s, merge);
				const auto slot = s.builder.AllocateId();
				s.builder.AddFunction(spv::OpPhi, u, slot, ConstantU32(s, C::MaxRecords), header,
				                      expected, attempt);
				(void)EmitValueOrDefaultIfCondition(s,
				    Binary(s, spv::OpULessThan, b, slot, capacity), u, ConstantU32(s, 0), [&] {
					const auto at = Binary(s, spv::OpIAdd, u, ConstantU32(s, base + C::HeaderWords),
					    Binary(s, spv::OpIMul, u, slot, ConstantU32(s, C::RecordWords)));
					const auto store = [&](uint32_t word, uint32_t value) {
						s.builder.AddFunction(spv::OpStore,
						    pointer(Binary(s, spv::OpIAdd, u, at, ConstantU32(s, word))), value);
					};
					store(C::Pc, pc);
					store(C::NodeLow, Unary(s, spv::OpUConvert, u, args[1]));
					store(C::NodeHigh, Unary(s, spv::OpUConvert, u,
					    Binary(s, spv::OpShiftRightLogical, TypeScalarU64(s), args[1], ConstantDeviceAddress(s, 32))));
					store(C::NodeWords, Select(s, u, resident,
					    Select(s, u, full, ConstantU32(s, 32), ConstantU32(s, 16)), ConstantU32(s, 0)));
					store(C::Status, status);
					for (uint32_t i = 0; i < 4; ++i) store(C::InstanceLow + i, Extract(s,u,context,i));
					for (uint32_t i = 0; i < 4; ++i) {
						store(C::Descriptor + i, Extract(s, u, args[0], i));
						store(C::Result + i, Extract(s, u, result, i));
					}
					store(C::Extent, Unary(s, spv::OpBitcast, u, args[2]));
					for (uint32_t v = 0; v < 3; ++v)
						for (uint32_t i = 0; i < 3; ++i)
							store(C::Origin + v * 3 + i,
							    Unary(s, spv::OpBitcast, u, Extract(s, TypeF32(s), args[3 + v], i)));
					(void)EmitValueOrDefaultIfCondition(s, resident, u, ConstantU32(s, 0), [&] {
						for (uint32_t i = 0; i < 16; ++i) store(C::Data + i, NodeWord(s, first, ConstantU32(s, i)));
						(void)EmitValueOrDefaultIfCondition(s, full, u, ConstantU32(s, 0), [&] {
							for (uint32_t i = 0; i < 16; ++i) store(C::Data + 16 + i, NodeWord(s, second, ConstantU32(s, i)));
							return ConstantU32(s, 0);
						});
						return ConstantU32(s, 0);
					});
					for (uint32_t i = 0; i < 3; ++i) store(C::Invocation + i, Extract(s, u, invocation, i));
					if (s.program.shader_hash == C::AstroShader) {
						const auto enabled = Binary(s,spv::OpIEqual,b,load(field(C::SceneEnabled),false),ConstantU32(s,1));
						const auto selected = Binary(s,spv::OpLogicalAnd,b,enabled,
						    Binary(s,spv::OpIEqual,b,pc,ConstantU32(s,0x520)));
						const auto ready = Binary(s,spv::OpLogicalAnd,b,selected,resident);
						(void)EmitValueOrDefaultIfCondition(s,ready,u,ConstantU32(s,0),[&] {
							CaptureAstroScene(s,args[0],context,slot,pc);
							return ConstantU32(s,0);
						});
					}
					return ConstantU32(s, 0);
				});
				return ConstantU32(s, 0);
			});
			return ConstantU32(s, 0);
		});
	});
}

uint32_t Triangle(EmitterState& s, uint32_t block, uint32_t kind, uint32_t bary,
                  Vec3 origin, Vec3 direction) {
	const auto f = TypeF32(s), u = TypeU32(s), b = TypeBool(s);
	const auto zero = ConstantF32Value(s, 0.0f);
	const auto op = [&](spv::Op code, uint32_t x, uint32_t y) {
		const auto value = Binary(s, code, f, x, y);
		s.builder.AddAnnotation(spv::OpDecorate, value, spv::DecorationNoContraction);
		return value;
	};
	const auto cmp = [&](spv::Op code, uint32_t x, uint32_t y) { return Binary(s, code, b, x, y); };
	const auto either = [&](uint32_t x, uint32_t y) { return Binary(s, spv::OpLogicalOr, b, x, y); };
	const auto both = [&](uint32_t x, uint32_t y) { return Binary(s, spv::OpLogicalAnd, b, x, y); };
	const auto abs = [&](uint32_t x) { return EmitGlsl<GLSLstd450FAbs, IR::Type::F32>(s, x); };
	const auto is_kind = [&](uint32_t n) { return cmp(spv::OpIEqual, kind, ConstantU32(s, n)); };
	// The four triangles are (0,1,2), (1,3,2), (2,3,4), (2,4,0).
	const std::array vertices {
		EmitGlsl<GLSLstd450UMin, IR::Type::U32>(s, kind, ConstantU32(s, 2)),
		Select(s, u, is_kind(0), ConstantU32(s, 1),
		       Select(s, u, is_kind(3), ConstantU32(s, 4), ConstantU32(s, 3))),
		Select(s, u, is_kind(2), ConstantU32(s, 4),
		       Select(s, u, is_kind(3), ConstantU32(s, 0), ConstantU32(s, 2))) };
	std::array<Vec3, 3> positions;
	for (uint32_t i = 0; i < 3; ++i) {
		const auto first = Binary(s, spv::OpIMul, u, vertices[i], ConstantU32(s, 3));
		for (uint32_t axis = 0; axis < 3; ++axis)
			positions[i][axis] = Unary(s, spv::OpBitcast, f,
			    NodeWord(s, block, Binary(s, spv::OpIAdd, u, first, ConstantU32(s, axis))));
	}
	const auto flag = NodeWord(s, block, ConstantU32(s, 15));
	const auto y_largest = cmp(spv::OpFOrdLessThan, abs(direction[0]), abs(direction[1]));
	const auto z_largest = cmp(spv::OpFOrdLessThan,
	    Select(s, f, y_largest, abs(direction[1]), abs(direction[0])), abs(direction[2]));
	const auto rotate = [&](Vec3 value) {
		Vec3 result;
		for (uint32_t i = 0; i < 3; ++i)
			result[i] = Select(s, f, z_largest, value[i],
			    Select(s, f, y_largest, value[(i + 2) % 3], value[(i + 1) % 3]));
		return result;
	};
	origin = rotate(origin);
	direction = rotate(direction);
	std::array<Vec3, 3> projected;
	for (uint32_t i = 0; i < 3; ++i) {
		const auto position = rotate(positions[i]);
		Vec3 relative;
		for (uint32_t axis = 0; axis < 3; ++axis)
			relative[axis] = op(spv::OpFSub, position[axis], origin[axis]);
		projected[i] = {op(spv::OpFSub, op(spv::OpFMul, relative[0], direction[2]),
		                              op(spv::OpFMul, direction[0], relative[2])),
		                op(spv::OpFSub, op(spv::OpFMul, relative[1], direction[2]),
		                              op(spv::OpFMul, direction[1], relative[2])), relative[2]};
	}
	Vec3 edge, weighted;
	for (uint32_t i = 0; i < 3; ++i) {
		const auto& first = projected[(i + 1) % 3];
		const auto& second = projected[(i + 2) % 3];
		edge[i] = op(spv::OpFSub, op(spv::OpFMul, second[0], first[1]),
		                        op(spv::OpFMul, second[1], first[0]));
		weighted[i] = op(spv::OpFMul, edge[i], direction[2]);
	}
	auto numerator = op(spv::OpFAdd, op(spv::OpFAdd,
	    op(spv::OpFMul, edge[0], projected[0][2]), op(spv::OpFMul, edge[1], projected[1][2])),
	    op(spv::OpFMul, edge[2], projected[2][2]));
	auto denominator = op(spv::OpFAdd, op(spv::OpFAdd, weighted[0], weighted[1]), weighted[2]);
	uint32_t negative = ConstantBool(s, false), positive = negative;
	for (auto value: edge) {
		negative = either(negative, cmp(spv::OpFOrdLessThan, value, zero));
		positive = either(positive, cmp(spv::OpFOrdGreaterThan, value, zero));
	}
	const auto winding = cmp(spv::OpFOrdGreaterThan, denominator, zero);
	auto missed = either(both(negative, positive), cmp(spv::OpFOrdEqual, denominator, zero));
	missed = either(missed, Unary(s, spv::OpIsNan, b, numerator));
	missed = either(missed, cmp(spv::OpFOrdLessThan,
	    Select(s, f, winding, numerator, Unary(s, spv::OpFNegate, f, numerator)), zero));
	for (uint32_t i = 0; i < 3; ++i) {
		const auto first = projected[(i + 1) % 3][1], second = projected[(i + 2) % 3][1];
		const auto first_zero = cmp(spv::OpFOrdEqual, first, zero);
		const auto horizontal = both(first_zero, cmp(spv::OpFOrdEqual, second, zero));
		const auto below = either(cmp(spv::OpFOrdLessThan, first, zero),
		    both(first_zero, cmp(spv::OpFOrdGreaterThan, second, zero)));
		const auto right = Binary(s, spv::OpLogicalNotEqual, b, below, winding);
		const auto excluded = Select(s, b, horizontal,
		    cmp(spv::OpFOrdGreaterThan, projected[i][1], zero), right);
		missed = either(missed, both(cmp(spv::OpFOrdEqual, edge[i], zero), excluded));
	}
	const auto remap = Binary(s, spv::OpShiftRightLogical, u, flag,
	    Binary(s, spv::OpIMul, u, kind, ConstantU32(s, 8)));
	const auto coordinate = [&](uint32_t shift) {
		const auto index = Binary(s, spv::OpBitwiseAnd, u,
		    Binary(s, spv::OpShiftRightLogical, u, remap, ConstantU32(s, shift)), ConstantU32(s, 3));
		return Unary(s, spv::OpBitcast, u, Select(s, f,
		    cmp(spv::OpIEqual, index, ConstantU32(s, 1)), weighted[1],
		    Select(s, f, cmp(spv::OpIEqual, index, ConstantU32(s, 2)), weighted[2], weighted[0])));
	};
	const std::array result {
		Unary(s, spv::OpBitcast, u, Select(s, f, missed, ConstantF32(s, 0x7f800000), numerator)),
		Unary(s, spv::OpBitcast, u, Select(s, f, missed, ConstantF32Value(s, 1.0f), denominator)),
		Select(s, u, bary, coordinate(0), Binary(s, spv::OpIAdd, u, flag, kind)),
		Select(s, u, bary, coordinate(2), Select(s, u, missed, ConstantU32(s, 0), ConstantU32(s, 1))) };
	return Vector(s, TypeU32Vector(s, 4), result);
}

std::array<uint32_t, 6> Box(EmitterState& s, uint32_t first, uint32_t second,
                           uint32_t half, uint32_t child) {
	const auto f = TypeF32(s);
	const auto half_label = s.builder.AllocateId(), full_label = s.builder.AllocateId();
	const auto merge = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpSelectionMerge, merge, spv::SelectionControlMaskNone);
	s.builder.AddFunction(spv::OpBranchConditional, half, half_label, full_label);
	EmitLabel(s, half_label);
	std::array<uint32_t, 6> packed, full, result;
	for (uint32_t pair = 0; pair < 3; ++pair) {
		const auto word = NodeWord(s, first, ConstantU32(s, 4 + child * 3 + pair));
		const auto unpacked = EmitGlsl<GLSLstd450UnpackHalf2x16, IR::Type::F32x2>(s, word);
		packed[pair * 2] = Extract(s, f, unpacked, 0);
		packed[pair * 2 + 1] = Extract(s, f, unpacked, 1);
	}
	s.builder.AddFunction(spv::OpBranch, merge);
	EmitLabel(s, full_label);
	for (uint32_t component = 0; component < 6; ++component) {
		const auto word = 4 + child * 6 + component;
		full[component] = Unary(s, spv::OpBitcast, f,
		    NodeWord(s, word < 16 ? first : second, ConstantU32(s, word % 16)));
	}
	s.builder.AddFunction(spv::OpBranch, merge);
	EmitLabel(s, merge);
	for (uint32_t component = 0; component < 6; ++component) {
		result[component] = s.builder.AllocateId();
		s.builder.AddFunction(spv::OpPhi, f, result[component], packed[component], half_label,
		                      full[component], full_label);
	}
	return result;
}

uint32_t Boxes(EmitterState& s, uint32_t first, uint32_t second, uint32_t half,
               uint32_t grow, uint32_t sort, uint32_t extent, const Vec3& origin, const Vec3& inverse) {
	const auto f = TypeF32(s), u = TypeU32(s), b = TypeBool(s);
	const auto zero = ConstantF32Value(s, 0.0f);
	std::array<uint32_t, 4> depths, indices;
	Vec3 forward;
	for (uint32_t axis = 0; axis < 3; ++axis)
		forward[axis] = Binary(s, spv::OpFOrdGreaterThanEqual, b, inverse[axis], zero);
	for (uint32_t child = 0; child < 4; ++child) {
		const auto box = Box(s, first, second, half, child);
		Vec3 near, far;
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const auto lo = Binary(s, spv::OpFMul, f,
			    Binary(s, spv::OpFSub, f, box[axis], origin[axis]), inverse[axis]);
			const auto hi = Binary(s, spv::OpFMul, f,
			    Binary(s, spv::OpFSub, f, box[axis + 3], origin[axis]), inverse[axis]);
			near[axis] = Select(s, f, forward[axis], lo, hi);
			far[axis] = Select(s, f, forward[axis], hi, lo);
		}
		const auto entry = EmitGlsl<GLSLstd450FMax, IR::Type::F32>(s,
		    EmitGlsl<GLSLstd450FMax, IR::Type::F32>(s, near[0], near[1]), near[2]);
		const auto exit = EmitGlsl<GLSLstd450FMin, IR::Type::F32>(s,
		    EmitGlsl<GLSLstd450FMin, IR::Type::F32>(s, far[0], far[1]), far[2]);
		// Valid hits have nonnegative exit distance. Saturate ULP growth at +infinity.
		const auto bits = Unary(s, spv::OpBitcast, u, Select(s, f,
		    Binary(s, spv::OpFOrdEqual, b, exit, zero), zero, exit));
		const auto grown_bits = EmitGlsl<GLSLstd450UMin, IR::Type::U32>(s,
		    Binary(s, spv::OpIAdd, u, bits, grow), ConstantU32(s, 0x7f800000));
		const auto grown = Unary(s, spv::OpBitcast, f, grown_bits);
		auto hit = Binary(s, spv::OpLogicalAnd, b,
		    Binary(s, spv::OpFOrdGreaterThanEqual, b, exit, zero),
		    Binary(s, spv::OpFOrdLessThan, b, entry, extent));
		hit = Binary(s, spv::OpLogicalAnd, b, hit,
		    Binary(s, spv::OpFOrdLessThanEqual, b, entry, grown));
		depths[child] = Unary(s, spv::OpBitcast, u, Select(s, f, hit,
		    Select(s, f, Binary(s, spv::OpFOrdGreaterThan, b, entry, zero), entry, zero),
		    ConstantF32(s, 0x7f800000)));
		indices[child] = Select(s, u, hit, NodeWord(s, first, ConstantU32(s, child)), ConstantU32(s, ~0u));
	}
	// A select network keeps the original child order when sorting is disabled.
	constexpr std::array<std::pair<uint32_t, uint32_t>, 5> network {{{0,1}, {2,3}, {0,2}, {1,3}, {1,2}}};
	for (size_t step = 0; step < network.size(); ++step) {
		const auto [a, c] = network[step];
		const auto swap = Binary(s, spv::OpLogicalAnd, b, sort,
		    Binary(s, spv::OpUGreaterThan, b, depths[a], depths[c]));
		const auto depth = depths[a], index = indices[a];
		const auto later = [&](uint32_t position) {
			return std::ranges::any_of(std::span(network).subspan(step + 1), [=](auto pair) {
				return pair.first == position || pair.second == position;
			});
		};
		if (later(a)) depths[a] = Select(s, u, swap, depths[c], depth);
		if (later(c)) depths[c] = Select(s, u, swap, depth, depths[c]);
		indices[a] = Select(s, u, swap, indices[c], index);
		indices[c] = Select(s, u, swap, index, indices[c]);
	}
	return Vector(s, TypeU32Vector(s, 4), indices);
}

} // namespace

void DefineBvhIntersect(EmitterState& s) {
	if (!s.requirements.bvh) return;
	const auto u = TypeU32(s), f = TypeF32(s), b = TypeBool(s), wide = TypeScalarU64(s);
	const auto vec4 = TypeU32Vector(s, 4), vec3 = TypeF32Vector(s, 3);
	const bool capture = CaptureApplies(s);
	const auto signature = capture
	    ? s.builder.Type(spv::OpTypeFunction, vec4, vec4, wide, f, vec3, vec3, vec3, u, TypeU32Vector(s, 3), vec4)
	    : s.builder.Type(spv::OpTypeFunction, vec4, vec4, wide, f, vec3, vec3, vec3);
	s.bvh_intersect_function = s.builder.AllocateId();
	s.builder.AddName(s.bvh_intersect_function, "bvh_intersect");
	s.builder.AddFunction(spv::OpFunction, vec4, s.bvh_intersect_function,
	                      spv::FunctionControlMaskNone, signature);
	std::array<uint32_t, 6> args;
	const std::array types {vec4, wide, f, vec3, vec3, vec3};
	for (uint32_t i = 0; i < args.size(); ++i) {
		args[i] = s.builder.AllocateId();
		s.builder.AddFunction(spv::OpFunctionParameter, types[i], args[i]);
	}
	uint32_t pc = 0, invocation = 0, context = 0;
	if (capture) {
		pc = s.builder.AllocateId();
		s.builder.AddFunction(spv::OpFunctionParameter, u, pc);
		invocation = s.builder.AllocateId();
		s.builder.AddFunction(spv::OpFunctionParameter, TypeU32Vector(s, 3), invocation);
		context = s.builder.AllocateId();
		s.builder.AddFunction(spv::OpFunctionParameter, vec4, context);
	}
	EmitLabel(s, s.builder.AllocateId());
	const auto descriptor = args[0], node = args[1], extent = args[2];
	Vec3 origin, direction, inverse;
	for (uint32_t i = 0; i < 3; ++i) {
		origin[i] = Extract(s, f, args[3], i);
		direction[i] = Extract(s, f, args[4], i);
		inverse[i] = Extract(s, f, args[5], i);
	}
	std::array<uint32_t, 4> words;
	for (uint32_t i = 0; i < 4; ++i) words[i] = Extract(s, u, descriptor, i);
	const auto and_bits = [&](uint32_t value, uint32_t mask) {
		return Binary(s, spv::OpBitwiseAnd, u, value, ConstantU32(s, mask));
	};
	const auto shr = [&](uint32_t value, uint32_t shift) {
		return Binary(s, spv::OpShiftRightLogical, u, value, ConstantU32(s, shift));
	};
	const auto kind = and_bits(Unary(s, spv::OpUConvert, u, node), 7);
	const auto triangle = Binary(s, spv::OpULessThan, b, kind, ConstantU32(s, 4));
	const auto full = Binary(s, spv::OpIEqual, b, kind, ConstantU32(s, 5));
	const auto half = Binary(s, spv::OpIEqual, b, kind, ConstantU32(s, 4));
	const auto bary = Binary(s, spv::OpINotEqual, b, and_bits(words[3], 0x01000000), ConstantU32(s, 0));
	const auto sort = Binary(s, spv::OpINotEqual, b, and_bits(words[1], 0x80000000), ConstantU32(s, 0));
	const auto grow = and_bits(shr(words[1], 23), 0xff);
	const auto base = Binary(s, spv::OpShiftLeftLogical, wide,
	    DeviceAddressFromWords(s, words[0], and_bits(words[1], 0xff)), ConstantDeviceAddress(s, 8));
	const auto index = Binary(s, spv::OpShiftRightLogical, wide, node, ConstantDeviceAddress(s, 3));
	const auto last_index = Binary(s, spv::OpIAdd, wide, index,
	    Select(s, wide, full, ConstantDeviceAddress(s, 1), ConstantDeviceAddress(s, 0)));
	const auto address = Binary(s, spv::OpIAdd, wide, base,
	    Binary(s, spv::OpShiftLeftLogical, wide, index, ConstantDeviceAddress(s, 6)));
	const auto last_address = Binary(s, spv::OpIAdd, wide, base,
	    Binary(s, spv::OpShiftLeftLogical, wide, last_index, ConstantDeviceAddress(s, 6)));
	auto valid = Binary(s, spv::OpIEqual, b, shr(words[3], 28), ConstantU32(s, 8));
	const auto require = [&](uint32_t condition) {
		valid = Binary(s, spv::OpLogicalAnd, b, valid, condition);
	};
	require(Binary(s, spv::OpULessThanEqual, b, kind, ConstantU32(s, 5)));
	require(Binary(s, spv::OpULessThanEqual, b, last_index, DeviceAddressFromWords(s, words[2], and_bits(words[3], 0x3ff))));
	require(Binary(s, spv::OpULessThan, b, last_address, ConstantDeviceAddress(s, BufferCache::CACHING_NUMPAGES * BufferCache::CACHING_PAGESIZE)));
	require(Unary(s, spv::OpLogicalNot, b, Unary(s, spv::OpIsNan, b, extent)));
	for (uint32_t i = 0; i < 3; ++i) {
		for (const auto value: {origin[i], direction[i], inverse[i]})
			require(Unary(s, spv::OpLogicalNot, b, Unary(s, spv::OpIsNan, b, value)));
		require(Unary(s, spv::OpLogicalNot, b, Unary(s, spv::OpIsInf, b, origin[i])));
	}
	const std::array triangle_invalid {ConstantU32(s, 0x7f800000), ConstantU32(s, 0x3f800000),
	                                  ConstantU32(s, ~0u), ConstantU32(s, 0)};
	const std::array box_invalid {ConstantU32(s, ~0u), ConstantU32(s, ~0u),
	                             ConstantU32(s, ~0u), ConstantU32(s, ~0u)};
	std::array<uint32_t, 4> invalid_words;
	for (uint32_t i = 0; i < 4; ++i)
		invalid_words[i] = Select(s, u, triangle, triangle_invalid[i], box_invalid[i]);
	const auto invalid = Vector(s, vec4, invalid_words);
	// The diagnostic carries the guarded addresses alongside the unchanged
	// software result, so failed inputs can be recorded without any node load.
	const auto value_type = capture ? s.builder.Type(spv::OpTypeStruct, vec4, wide, wide, u) : vec4;
	const auto bundle = [&](uint32_t value, uint32_t first, uint32_t second, uint32_t status) {
		if (!capture) return value;
		const std::array members{value, first, second, ConstantU32(s, status)};
		return Vector(s, value_type, members);
	};
	const auto zero_address = ConstantDeviceAddress(s, 0);
	const auto default_value = bundle(invalid, zero_address, zero_address, BvhCapture::InvalidInput);
	const auto result = EmitValueOrDefaultIfCondition(s, valid, value_type, default_value, [&] {
		const auto first = GetBdaPointer(s, address);
		const auto second = EmitValueOrDefaultIfCondition(s, full, wide, ConstantDeviceAddress(s, 0), [&] {
			return GetBdaPointer(s, Binary(s, spv::OpIAdd, wide, address, ConstantDeviceAddress(s, 64)));
		});
		const auto present = Binary(s, spv::OpLogicalAnd, b,
		    Binary(s, spv::OpINotEqual, b, first, ConstantDeviceAddress(s, 0)),
		    Binary(s, spv::OpLogicalOr, b, Unary(s, spv::OpLogicalNot, b, full),
		        Binary(s, spv::OpINotEqual, b, second, ConstantDeviceAddress(s, 0))));
		const auto unmapped = bundle(invalid, zero_address, zero_address, BvhCapture::Unmapped);
		return EmitValueOrDefaultIfCondition(s, present, value_type, unmapped, [&] {
			const auto tri_label = s.builder.AllocateId(), box_label = s.builder.AllocateId();
			const auto merge = s.builder.AllocateId();
			s.builder.AddFunction(spv::OpSelectionMerge, merge, spv::SelectionControlMaskNone);
			s.builder.AddFunction(spv::OpBranchConditional, triangle, tri_label, box_label);
			EmitLabel(s, tri_label);
			const auto tri_value = Triangle(s, first, kind, bary, origin, direction);
			const auto tri_exit = s.current_label;
			s.builder.AddFunction(spv::OpBranch, merge);
			EmitLabel(s, box_label);
			const auto box_value = Boxes(s, first, second, half, grow, sort, extent, origin, inverse);
			const auto box_exit = s.current_label;
			s.builder.AddFunction(spv::OpBranch, merge);
			EmitLabel(s, merge);
			const auto value = s.builder.AllocateId();
			s.builder.AddFunction(spv::OpPhi, vec4, value, tri_value, tri_exit, box_value, box_exit);
			return bundle(value, first, second, BvhCapture::Resident);
		});
	});
	if (capture) {
		const auto value = Extract(s, vec4, result, 0);
		CaptureNode(s, args, pc, invocation, Extract(s, wide, result, 1), Extract(s, wide, result, 2),
		    full, Extract(s, u, result, 3), value, context);
		s.builder.AddFunction(spv::OpReturnValue, value);
	} else s.builder.AddFunction(spv::OpReturnValue, result);
	s.builder.AddFunction(spv::OpFunctionEnd);
}

#include "spirvEmitterAstroRt.inc"

uint32_t EmitBvhIntersect(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& s = ctx.state;
	const auto* ray = ctx.ImageAddress(inst.Arg(1));
	if (ray == nullptr || ray->NumArgs() < 12 || s.bvh_intersect_function == 0)
		ctx.Fail(inst, "BVH intersection requires normalized ray/node components and its shared function");
	const auto vector = [&](uint32_t first) {
		Vec3 values;
		for (uint32_t i = 0; i < 3; ++i)
			values[i] = Unary(s, spv::OpBitcast, TypeF32(s), ctx.Arg(*ray, first + i));
		return Vector(s, TypeF32Vector(s, 3), values);
	};
	const auto descriptor = ctx.Arg(inst, 0);
	const auto node = DeviceAddressFromWords(s, ctx.Arg(*ray, 0), ctx.Arg(*ray, 11));
	const auto extent = Unary(s, spv::OpBitcast, TypeF32(s), ctx.Arg(*ray, 1));
	const auto origin = vector(2), direction = vector(5), inverse = vector(8);
	return EmitValueOrDefaultIfCondition(s, ctx.Arg(inst, 2), TypeU32Vector(s, 4),
	    ConstantU32CompositeZero(s, 4), [&] {
		const auto result = s.builder.AllocateId();
		if (CaptureApplies(s)) {
			std::array<uint32_t, 3> ids;
			uint32_t divisor = 1;
			for (uint32_t axis = 0; axis < 3; ++axis) {
				if (s.lane_count == 1) {
					ids[axis] = EmitInputComponentU32(s, IR::StageInputKind::GlobalInvocationId, axis);
					continue;
				}
				// Match EmitBuiltinU32's virtual workgroup coordinates when a
				// guest wave64 executes as two halves on a host wave32.
				const auto* cs = ShaderWorkgroupInput(s.program.stage, s.input_info);
				const auto size = std::max(cs->threads_num[axis], 1u);
				const auto local = Binary(s, spv::OpUMod, TypeU32(s),
				    Binary(s, spv::OpUDiv, TypeU32(s), EmitLocalInvocationIndex(s), ConstantU32(s, divisor)),
				    ConstantU32(s, size));
				ids[axis] = Binary(s, spv::OpIAdd, TypeU32(s), local,
				    Binary(s, spv::OpIMul, TypeU32(s),
				        EmitInputComponentU32(s, IR::StageInputKind::WorkgroupId, axis), ConstantU32(s, size)));
				divisor *= size;
			}
			s.builder.AddFunction(spv::OpFunctionCall, TypeU32Vector(s, 4), result,
			    s.bvh_intersect_function, descriptor, node, extent, origin, direction, inverse,
			    ConstantU32(s, inst.Flags<uint32_t>()), Vector(s, TypeU32Vector(s, 3), ids), ctx.Arg(inst,3));
		}
		else s.builder.AddFunction(spv::OpFunctionCall, TypeU32Vector(s, 4), result,
		    s.bvh_intersect_function, descriptor, node, extent, origin, direction, inverse);
		return result;
	});
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
