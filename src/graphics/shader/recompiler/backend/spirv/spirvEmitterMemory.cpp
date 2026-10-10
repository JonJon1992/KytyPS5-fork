#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/shader/recompiler/CodegenOptions.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t AndCondition(EmitterState& state, uint32_t lhs, uint32_t rhs) {
	return Binary(state, spv::OpLogicalAnd, TypeBool(state), lhs, rhs);
}

uint32_t EmitDsMaskedLaneRead(EmitterState& state, uint32_t source, uint32_t target,
                              uint32_t exec) {
	if (state.lane_count == 2) {
		target = Binary(state, spv::OpBitwiseAnd, TypeU32(state), target, ConstantU32(state, 31));
	}
	const auto shuffled = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, TypeU32(state), shuffled,
	                          ConstantU32(state, spv::ScopeSubgroup), source, target);
	const auto source_exec = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, TypeBool(state), source_exec,
	                          ConstantU32(state, spv::ScopeSubgroup), exec, target);
	const auto source_active =
	    AndCondition(state, source_exec, EmitSubgroupLaneActiveBool(state, target));
	return Select(state, TypeU32(state), source_active, shuffled, ConstantU32(state, 0));
}

struct BufferAddress {
	uint32_t offset;
	uint32_t byte;
};

BufferAddress CalculateBufferAddress(EmitterState& state, uint32_t index, uint32_t offset,
                                     uint32_t soffset, uint32_t immediate, uint32_t stride,
                                     uint32_t swizzle, uint32_t index_stride) {
	const auto zero = ConstantU32(state, 0);
	const auto one  = ConstantU32(state, 1);
	const auto add = [&](uint32_t lhs, uint32_t rhs) {
		return lhs == zero ? rhs : rhs == zero ? lhs
		                                      : Binary(state, spv::OpIAdd, TypeU32(state), lhs, rhs);
	};
	const auto mul = [&](uint32_t lhs, uint32_t rhs) {
		return lhs == zero || rhs == zero ? zero
		       : lhs == one              ? rhs
		       : rhs == one              ? lhs
		                                 : Binary(state, spv::OpIMul, TypeU32(state), lhs, rhs);
	};
	if (immediate != 0u) {
		offset = add(offset, ConstantU32(state, immediate));
	}
	auto address = add(mul(index, stride), offset);
	if (swizzle != 0u) {
		const auto index_shift =
		    Binary(state, spv::OpIAdd, TypeU32(state), index_stride, ConstantU32(state, 3));
		const auto indices = Binary(state, spv::OpShiftLeftLogical, TypeU32(state),
		                            ConstantU32(state, 1), index_shift);
		const auto index_msb =
		    Binary(state, spv::OpShiftRightLogical, TypeU32(state), index, index_shift);
		const auto index_lsb =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), index,
		           Binary(state, spv::OpISub, TypeU32(state), indices, ConstantU32(state, 1)));
		const auto offset_msb =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), offset, ConstantU32(state, ~3u));
		const auto offset_lsb =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), offset, ConstantU32(state, 3u));
		const auto msb = mul(add(mul(index_msb, stride), offset_msb), indices);
		const auto lsb = add(Binary(state, spv::OpShiftLeftLogical, TypeU32(state), index_lsb,
		                            ConstantU32(state, 2u)), offset_lsb);
		address = Select(state, TypeU32(state), swizzle, add(msb, lsb), address);
	}
	return {offset, add(address, soffset)};
}

uint32_t BufferLane(EmitterState& state) {
	return Binary(state, spv::OpBitwiseAnd, TypeU32(state), EmitSubgroupLocalInvocationId(state),
	              ConstantU32(state, 63));
}

uint32_t BufferByteAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	auto&      state  = ctx.state;
	const auto packed = StorageBufferPackedStride(state, mem);
	const auto stride = packed & 0x3fffu;
	auto       index  = ctx.Arg(inst, 1);
	if ((packed & (1u << 20u)) != 0u) {
		index = Binary(state, spv::OpIAdd, TypeU32(state), index, BufferLane(state));
	}
	const bool swizzle = stride != 0u && (packed & (1u << 14u)) != 0u;
	const bool runtime = (packed & IR::PackedStrideRuntime) != 0u;
	return CalculateBufferAddress(state, index, ctx.Arg(inst, 2), ctx.Arg(inst, 3), mem.offset,
	                              runtime ? state.memory_strides[mem.resource]
	                                      : ConstantU32(state, stride),
	                              swizzle ? ConstantBool(state, true) : 0u,
	                              ConstantU32(state, (packed >> 16u) & 3u))
	    .byte;
}

uint32_t AddU64Low(EmitterState& state, uint32_t low, uint32_t high, uint32_t add_low,
                   uint32_t add_high, uint32_t& out_high) {
	const auto result = Binary(state, spv::OpIAdd, TypeU32(state), low, add_low);
	const auto carry  = Binary(state, spv::OpULessThan, TypeBool(state), result, low);
	out_high =
	    Binary(state, spv::OpIAdd, TypeU32(state),
	           Binary(state, spv::OpIAdd, TypeU32(state), high, add_high),
	           Select(state, TypeU32(state), carry, ConstantU32(state, 1), ConstantU32(state, 0)));
	return result;
}

uint32_t ScratchByteAddress(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t low,
                            uint32_t high) {
	auto& state     = ctx.state;
	auto  immediate = static_cast<int32_t>(mem.offset);
	const auto immediate_low  = ConstantU32(state, static_cast<uint32_t>(immediate));
	const auto immediate_high = ConstantU32(state, immediate < 0 ? UINT32_MAX : 0u);
	low                      = AddU64Low(state, low, high, immediate_low, immediate_high, high);
	const auto valid = Binary(state, spv::OpIEqual, TypeBool(state), high, ConstantU32(state, 0));
	return Select(state, TypeU32(state), valid, low, ConstantU32(state, UINT32_MAX));
}

} // namespace

uint32_t ConstantDeviceAddress(EmitterState& state, uint64_t value) {
	return state.builder.Constant(spv::OpConstant, TypeScalarU64(state),
	                              static_cast<uint32_t>(value),
	                              static_cast<uint32_t>(value >> 32u));
}

uint32_t DeviceAddressFromWords(EmitterState& state, uint32_t low, uint32_t high) {
	const auto low64  = Unary(state, spv::OpUConvert, TypeScalarU64(state), low);
	const auto high64 = Binary(state, spv::OpShiftLeftLogical, TypeScalarU64(state),
	                           Unary(state, spv::OpUConvert, TypeScalarU64(state), high),
	                           ConstantDeviceAddress(state, 32));
	return Binary(state, spv::OpBitwiseOr, TypeScalarU64(state), low64, high64);
}

namespace {

uint32_t GuestAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	auto& state = ctx.state;
	auto  low   = ctx.Arg(inst, 1);
	if (mem.kind == IR::ResourceKind::ScalarAddress) {
		low = Binary(state, spv::OpBitwiseAnd, TypeU32(state), low, ConstantU32(state, ~3u));
	}
	uint32_t address = 0;
	if (mem.address_is_full) {
		address = DeviceAddressFromWords(state, low, ctx.Arg(inst, 2));
	} else {
		const auto* handle = inst.Arg(0).Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != IR::ValueOpcode::GetAddressResource ||
		    handle->NumArgs() != 2) {
			ctx.Fail(inst, "has no address base pair");
			return ConstantDeviceAddress(state, 0);
		}
		auto base_low = ctx.Arg(*handle, 0);
		if (mem.kind == IR::ResourceKind::ScalarAddress) {
			base_low = Binary(state, spv::OpBitwiseAnd, TypeU32(state), base_low,
			                  ConstantU32(state, ~3u));
		}
		const auto base = DeviceAddressFromWords(state, base_low, ctx.Arg(*handle, 1));
		address         = Binary(state, spv::OpIAdd, TypeScalarU64(state), base,
		                         Unary(state, spv::OpUConvert, TypeScalarU64(state), low));
	}
	auto immediate = static_cast<int32_t>(mem.offset);
	if (mem.kind == IR::ResourceKind::ScalarAddress) {
		immediate = static_cast<int32_t>(static_cast<uint32_t>(immediate) & ~3u);
	}
	return immediate == 0
	           ? address
	           : Binary(state, spv::OpIAdd, TypeScalarU64(state), address,
	                    ConstantDeviceAddress(
	                        state, static_cast<uint64_t>(static_cast<int64_t>(immediate))));
}

uint32_t FaultElementPointer(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
	                          state.fault_buffer_variable, ConstantU32(state, 0), index);
	return pointer;
}

void RecordBdaFault(EmitterState& state, uint32_t page) {
	const auto word =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), page, ConstantU32(state, 5));
	const auto bit =
	    Binary(state, spv::OpShiftLeftLogical, TypeU32(state), ConstantU32(state, 1),
	           Binary(state, spv::OpBitwiseAnd, TypeU32(state), page, ConstantU32(state, 31)));
	// Atomic: lanes and waves faulting on pages that share a bitmap word must not lose bits
	// (a load/or/store sequence races and drops faults).
	const auto pointer = FaultElementPointer(state, word);
	const auto result  = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAtomicOr, TypeU32(state), result, pointer,
	                          ConstantU32(state, spv::ScopeDevice),
	                          ConstantU32(state, spv::MemorySemanticsMaskNone), bit);
}

uint32_t GetBdaPointer(ValueEmitContext& ctx, uint32_t address) {
	auto&      state  = ctx.state;
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunctionCall, TypeScalarU64(state), result,
	                          state.bda_pointer_function, address);
	return result;
}

uint32_t LoadBdaDword(ValueEmitContext& ctx, uint32_t address) {
	auto&      state   = ctx.state;
	const auto bda     = GetBdaPointer(ctx, address);
	const auto present =
	    Binary(state, spv::OpINotEqual, TypeBool(state), bda, ConstantDeviceAddress(state, 0));
	return EmitValueOrZeroIfCondition(state, present, [&]() {
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpConvertUToPtr, TypePhysicalU32Pointer(state), pointer,
		                          bda);
		const auto         value     = state.builder.AllocateId();
		constexpr uint32_t alignment = sizeof(uint32_t);
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer,
		                          spv::MemoryAccessAlignedMask, alignment);
		return value;
	});
}

uint32_t LoadBdaInline(ValueEmitContext& ctx, uint32_t address, uint32_t active, uint32_t bits) {
	auto& state = ctx.state;
	return EmitValueOrZeroIfCondition(state, active, [&]() {
		const auto aligned = Binary(state, spv::OpBitwiseAnd, TypeScalarU64(state), address,
		                            ConstantDeviceAddress(state, ~uint64_t {3}));
		const auto first   = LoadBdaDword(ctx, aligned);
		const auto byte =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state),
		           Unary(state, spv::OpUConvert, TypeU32(state), address), ConstantU32(state, 3));
		const auto crosses =
		    bits == 8u ? ConstantBool(state, false)
		               : Binary(state, bits == 16u ? spv::OpUGreaterThan : spv::OpINotEqual,
		                        TypeBool(state), byte, ConstantU32(state, bits == 16u ? 2u : 0u));
		const auto second = EmitValueOrZeroIfCondition(state, crosses, [&]() {
			return LoadBdaDword(ctx, Binary(state, spv::OpIAdd, TypeScalarU64(state), aligned,
			                                ConstantDeviceAddress(state, sizeof(uint32_t))));
		});
		const auto shift =
		    Binary(state, spv::OpShiftLeftLogical, TypeU32(state), byte, ConstantU32(state, 3));
		const auto upper_shift =
		    Binary(state, spv::OpShiftLeftLogical, TypeU32(state),
		           Binary(state, spv::OpBitwiseAnd, TypeU32(state),
		                  Binary(state, spv::OpISub, TypeU32(state), ConstantU32(state, 4), byte),
		                  ConstantU32(state, 3)),
		           ConstantU32(state, 3));
		const auto merged =
		    Binary(state, spv::OpBitwiseOr, TypeU32(state),
		           Binary(state, spv::OpShiftRightLogical, TypeU32(state), first, shift),
		           Binary(state, spv::OpShiftLeftLogical, TypeU32(state), second, upper_shift));
		return bits == 32u ? merged
		                   : Binary(state, spv::OpBitwiseAnd, TypeU32(state), merged,
		                            ConstantU32(state, bits == 8u ? 0xffu : 0xffffu));
	});
}

uint32_t LoadBda(ValueEmitContext& ctx, uint32_t address, uint32_t active, uint32_t bits) {
	auto& state = ctx.state;
	const auto function = state.bda_scalar_load_functions[bits == 8u ? 0 : bits == 16u ? 1 : 2];
	EXIT_IF(function == 0);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunctionCall, TypeU32(state), result, function, address, active);
	return result;
}

uint32_t ByteAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	if (mem.kind == IR::ResourceKind::Buffer) {
		return BufferByteAddress(ctx, inst, mem);
	}
	if (mem.kind == IR::ResourceKind::Lds || mem.kind == IR::ResourceKind::Gds) {
		if (mem.offset == 0u) {
			return ctx.Arg(inst, 0);
		}
		return Binary(ctx.state, spv::OpIAdd, TypeU32(ctx.state), ctx.Arg(inst, 0),
		              ConstantU32(ctx.state, mem.offset));
	}
	if (mem.kind != IR::ResourceKind::Scratch) {
		EXIT("physical address memory must use the BDA emitter\n");
	}
	return ScratchByteAddress(ctx, mem, ctx.Arg(inst, 1), ctx.Arg(inst, 2));
}

uint32_t DwordIndex(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	auto address = ByteAddress(ctx, inst, mem);
	if (mem.kind == IR::ResourceKind::Lds || mem.kind == IR::ResourceKind::Gds) {
		// RDNA2 DS region addresses retain bits [15:2] after adding the byte offset.
		address = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), address,
		                 ConstantU32(ctx.state, 0xffffu));
	}
	const auto index = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state),
	                          address, ConstantU32(ctx.state, 2));
	if (const auto slot = ctx.state.function_lds_slots.find(&inst);
	    slot != ctx.state.function_lds_slots.end()) {
		ctx.state.function_lds_index_slots.emplace(index, slot->second);
	}
	return index;
}

struct PreparedMemoryElement {
	MemoryResourceAccess resource;
	uint32_t             index = 0;
};

PreparedMemoryElement PrepareMemoryElement(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                           uint32_t raw_index) {
	auto       resource = PrepareMemoryResourceAccess(ctx.state, mem);
	const auto index    = EmitMemoryElementIndex(ctx.state, resource, raw_index);
	return {.resource = resource, .index = index};
}

uint32_t LoadWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                          uint32_t index);

uint32_t LoadSubwordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                             uint32_t address, uint32_t index, uint32_t bits, bool sign_extend);

// Plain loads through storage-buffer descriptors may use robustBufferAccess2 with the host's
// whole-dword range contract (HostBufferRobustness). This preserves the current emulation's
// descriptor-range semantics; the AMD pilot still rejects indices whose byte address may wrap.
// LDS,
// GDS, scratch, formatted (all-or-nothing) accesses, stores and atomics keep their explicit checks.
// KYTY_ROBUST_BUFFER_LOADS=0 keeps the explicit check everywhere.
bool DeviceChecksDwordLoad(const MemoryResourceAccess& resource) {
	return (resource.kind == IR::ResourceKind::Buffer ||
	        resource.kind == IR::ResourceKind::ScalarBuffer) &&
	       GetCodegenOptions().robust_buffer_loads &&
	       GetHostBufferRobustness().storage_dword_loads_return_zero;
}

uint32_t DeviceDwordLoadInBounds(EmitterState& state, uint32_t index) {
	if (!GetHostBufferRobustness().null_descriptor_for_short_ranges) {
		return ConstantBool(state, true); // Existing alignment-1 path.
	}
	// A dword index can exceed 2^30 after the descriptor's alignment prefix or a scalar
	// immediate is added. A driver using 32-bit byte offsets could wrap that into the buffer.
	return Binary(state, spv::OpULessThan, TypeBool(state), index,
	              ConstantU32(state, 0x40000000u));
}

uint32_t PlainLoadInBounds(EmitterState& state, const MemoryResourceAccess& resource,
                           uint32_t index) {
	return DeviceChecksDwordLoad(resource) ? DeviceDwordLoadInBounds(state, index)
	                                       : EmitMemoryElementInBounds(state, resource, index);
}

// `load()` if `in_bounds`, else zero. The device always enables robustBufferAccess, so a load
// through a storage-buffer descriptor at an out-of-bounds index cannot fault (it reads zero or
// some value of the buffer): it is issued unconditionally and only its value is replaced, instead
// of a branch and a phi per load. The AMD compiler's cost follows control flow; see "Branch-free
// storage-buffer loads" in docs/performance-amd.md. LDS, GDS and scratch have no such guarantee
// and keep the branch, as does KYTY_ROBUST_BUFFER_LOADS=0.
template <typename Fn>
uint32_t LoadOrZero(EmitterState& state, const MemoryResourceAccess& resource, uint32_t in_bounds,
                    Fn&& load) {
	const bool storage_buffer = resource.kind == IR::ResourceKind::Buffer ||
	                            resource.kind == IR::ResourceKind::ScalarBuffer;
	if (!storage_buffer || !GetCodegenOptions().robust_buffer_loads) {
		return EmitValueOrZeroIfCondition(state, in_bounds, std::forward<Fn>(load));
	}
	const auto value = load();
	if (in_bounds == state.constant_true) {
		return value;
	}
	return Select(state, TypeU32(state), in_bounds, value, ConstantU32(state, 0));
}

uint32_t LoadWordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          const MemoryResourceAccess& resource) {
	const auto index = EmitMemoryElementIndex(ctx.state, resource, DwordIndex(ctx, inst, mem));
	return LoadOrZero(ctx.state, resource, PlainLoadInBounds(ctx.state, resource, index),
	                  [&]() { return LoadWordInBounds(ctx, resource, index); });
}

uint32_t LoadWord(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return LoadWordPrepared(ctx, inst, mem, resource);
	});
}

uint32_t LoadSubwordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                             const MemoryResourceAccess& resource, uint32_t bits,
                             bool sign_extend) {
	const auto address   = ByteAddress(ctx, inst, mem);
	const auto raw_index = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), address,
	                              ConstantU32(ctx.state, 2));
	const auto index     = EmitMemoryElementIndex(ctx.state, resource, raw_index);
	return LoadOrZero(ctx.state, resource, PlainLoadInBounds(ctx.state, resource, index), [&]() {
		return LoadSubwordInBounds(ctx, resource, address, index, bits, sign_extend);
	});
}

uint32_t LoadWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                          uint32_t index) {
	const auto value   = ctx.state.builder.AllocateId();
	const auto pointer = EmitMemoryElementPointer(ctx.state, resource, index);
	ctx.state.builder.AddFunction(spv::OpLoad, TypeU32(ctx.state), value, pointer,
	                              resource.memory_access);
	return value;
}

uint32_t LoadSubwordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                             uint32_t address, uint32_t index, uint32_t bits, bool sign_extend) {
	const auto word = LoadWordInBounds(ctx, resource, index);
	const auto byte  = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), address,
	                          ConstantU32(ctx.state, 3));
	const auto shift = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state), byte,
	                          ConstantU32(ctx.state, 3));
	const auto value =
	    Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state),
	           Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), word, shift),
	           ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu));
	if (!sign_extend) return value;
	const auto left = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state), value,
	                         ConstantU32(ctx.state, 32u - bits));
	return Binary(ctx.state, spv::OpShiftRightArithmetic, TypeU32(ctx.state), left,
	              ConstantU32(ctx.state, 32u - bits));
}

uint32_t LoadSubword(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem, uint32_t bits,
                     bool sign_extend) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return LoadSubwordPrepared(ctx, inst, mem, resource, bits, sign_extend);
	});
}

Prospero::BufferFormat BufferFormat(const ValueEmitContext& ctx, const IR::MemoryInfo& mem) {
	return mem.typed ? Format::DecodeTBufferFormat(mem.data_format, mem.number_format)
	                 : StorageBufferFormat(ctx.state, mem);
}

IR::MemoryInfo RebaseFormattedComponent(IR::MemoryInfo mem, const Format::BufferFormatInfo& info,
                                        uint32_t component) {
	mem.offset += Format::GetFormatComponentByteOffset(info, component);
	mem.data_dwords     = 1u;
	mem.component_index = component;
	return mem;
}

IR::MemoryInfo RebaseRawComponent(IR::MemoryInfo mem, uint32_t component) {
	mem.offset += component * 4u;
	mem.data_dwords     = 1u;
	mem.component_index = component;
	return mem;
}

using Format::FormattedSource;
using Format::FormattedSourceKind;

FormattedSource ResolveFormattedSource(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                       const Format::BufferFormatInfo& info,
                                       uint32_t                        output_component) {
	if (mem.typed) {
		return output_component < info.component_count
		           ? FormattedSource {FormattedSourceKind::Memory, output_component}
		           : FormattedSource {};
	}
	const auto selector = GetDstSel(ctx.state.program.info.buffers[mem.resource].descriptor_swizzle,
	                                output_component);
	const auto source = Format::ResolveFormattedSource(info, selector);
	if (source.kind == FormattedSourceKind::Invalid) {
		ExitDescriptorBindingFailure(ctx.state, IR::DescriptorBindingKind::Buffers, mem.resource,
		                             "buffer descriptor has reserved dst_sel");
	}
	return source;
}

uint32_t FormattedConstant(ValueEmitContext& ctx, const Format::BufferFormatInfo& info,
                           FormattedSourceKind kind) {
	return ConstantU32(ctx.state, Format::FormattedConstantBits(info, kind));
}

template <typename LoadWordFn, typename LoadSubwordFn>
uint32_t LoadFormattedComponent(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                const Format::BufferFormatInfo& info,
                                uint32_t output_component, LoadWordFn&& load_word,
                                LoadSubwordFn&& load_subword) {
	const auto source = ResolveFormattedSource(ctx, mem, info, output_component);
	if (source.kind != FormattedSourceKind::Memory) {
		return FormattedConstant(ctx, info, source.kind);
	}
	const auto component = source.component;
	const auto bits      = info.component_bits[component];
	uint32_t   raw       = 0;
	if (info.packed_bitfield) {
		raw = load_word(component);
		const auto type =
		    IsSignedFormatComponent(info.type) ? TypeI32(ctx.state) : TypeU32(ctx.state);
		const auto source_value =
		    type == TypeI32(ctx.state) ? Unary(ctx.state, spv::OpBitcast, type, raw) : raw;
		const auto extracted = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(IsSignedFormatComponent(info.type) ? spv::OpBitFieldSExtract
		                                                                 : spv::OpBitFieldUExtract,
		                              type, extracted, source_value,
		                              ConstantU32(ctx.state, info.component_bit_offset[component]),
		                              ConstantU32(ctx.state, bits));
		raw = type == TypeI32(ctx.state)
		          ? Unary(ctx.state, spv::OpBitcast, TypeU32(ctx.state), extracted)
		          : extracted;
	} else if (bits == 32u) {
		raw = load_word(component);
	} else {
		raw = load_subword(component, bits, IsSignedFormatComponent(info.type));
	}
	return NormalizeFormatComponent(ctx.state, info, component, raw);
}

uint32_t FormattedLoadPrepared(ValueEmitContext& ctx, const IR::Inst& inst,
                               const IR::MemoryInfo& mem, uint32_t output_component,
                               const MemoryResourceAccess& resource) {
	const auto info = Format::GetFormatInfo(BufferFormat(ctx, mem));
	if (info.type == Format::ComponentType::Unknown) {
		return LoadWordPrepared(ctx, inst, RebaseRawComponent(mem, output_component), resource);
	}
	return LoadFormattedComponent(
	    ctx, mem, info, output_component,
	    [&](uint32_t component) {
		    return LoadWordPrepared(ctx, inst, RebaseFormattedComponent(mem, info, component),
		                            resource);
	    },
	    [&](uint32_t component, uint32_t bits, bool sign_extend) {
		    return LoadSubwordPrepared(ctx, inst, RebaseFormattedComponent(mem, info, component),
		                               resource, bits, sign_extend);
	    });
}

uint32_t FormattedLoad(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return FormattedLoadPrepared(ctx, inst, mem, 0u, resource);
	});
}

void StoreSubwordInBounds(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                          const MemoryResourceAccess& resource, uint32_t address, uint32_t index,
                          uint32_t bits, uint32_t data) {
	const auto pointer = EmitMemoryElementPointer(ctx.state, resource, index);
	const auto shift   = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state),
	                            Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), address,
	                                   ConstantU32(ctx.state, 3)),
	                            ConstantU32(ctx.state, 3));
	const auto mask    = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state),
	                            ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu), shift);
	const auto value   = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state),
	                            Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), data,
	                                   ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu)),
	                            shift);
	const auto merge   = [&](uint32_t old) {
		return Binary(ctx.state, spv::OpBitwiseOr, TypeU32(ctx.state),
		              Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), old,
		                     Unary(ctx.state, spv::OpNot, TypeU32(ctx.state), mask)),
		              value);
	};
	if (mem.kind == IR::ResourceKind::Scratch) {
		const auto old = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpLoad, TypeU32(ctx.state), old, pointer);
		ctx.state.builder.AddFunction(spv::OpStore, pointer, merge(old));
	} else {
		AtomicUpdate(ctx.state, pointer, mem.kind, merge);
	}
}

void StoreSubwordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          const MemoryResourceAccess& resource, uint32_t bits, uint32_t data) {
	const auto address   = ByteAddress(ctx, inst, mem);
	const auto raw_index = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), address,
	                              ConstantU32(ctx.state, 2));
	const auto index     = EmitMemoryElementIndex(ctx.state, resource, raw_index);
	EmitIfCondition(
	    ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index), [&]() {
		    StoreSubwordInBounds(ctx, mem, resource, address, index, bits, data);
	    });
}

void StoreSubword(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem, uint32_t bits) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		StoreSubwordPrepared(ctx, inst, mem, resource, bits, ctx.Arg(inst, inst.NumArgs() - 2));
	});
}

void StoreWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource, uint32_t index,
                       uint32_t data) {
	ctx.state.builder.AddFunction(spv::OpStore,
	                              EmitMemoryElementPointer(ctx.state, resource, index), data,
	                              resource.memory_access);
}

void StoreWordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                       const MemoryResourceAccess& resource, uint32_t data) {
	const auto index = EmitMemoryElementIndex(ctx.state, resource, DwordIndex(ctx, inst, mem));
	EmitIfCondition(ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index), [&]() {
		StoreWordInBounds(ctx, resource, index, data);
	});
}

void StoreWord(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		StoreWordPrepared(ctx, inst, mem, resource, ctx.Arg(inst, inst.NumArgs() - 2));
	});
}

spv::Op SpirvAtomicOpcode(IR::ValueOpcode opcode) {
	switch (opcode) {
		case IR::ValueOpcode::BufferAtomicCmpSwap32: return spv::OpAtomicCompareExchange;
		case IR::ValueOpcode::BufferAtomicSwap32:
		case IR::ValueOpcode::BufferAtomicSwap64:
		case IR::ValueOpcode::SharedAtomicSwap32: return spv::OpAtomicExchange;
		case IR::ValueOpcode::BufferAtomicIAdd32:
		case IR::ValueOpcode::BufferAtomicIAdd64:
		case IR::ValueOpcode::SharedAtomicIAdd64:
		case IR::ValueOpcode::SharedAtomicIAdd32: return spv::OpAtomicIAdd;
		case IR::ValueOpcode::BufferAtomicISub32:
		case IR::ValueOpcode::BufferAtomicISub64:
		case IR::ValueOpcode::SharedAtomicISub32: return spv::OpAtomicISub;
		case IR::ValueOpcode::BufferAtomicSMin32:
		case IR::ValueOpcode::BufferAtomicSMin64:
		case IR::ValueOpcode::SharedAtomicSMin32: return spv::OpAtomicSMin;
		case IR::ValueOpcode::BufferAtomicUMin32:
		case IR::ValueOpcode::BufferAtomicUMin64:
		case IR::ValueOpcode::SharedAtomicUMin32: return spv::OpAtomicUMin;
		case IR::ValueOpcode::BufferAtomicSMax32:
		case IR::ValueOpcode::BufferAtomicSMax64:
		case IR::ValueOpcode::SharedAtomicSMax32: return spv::OpAtomicSMax;
		case IR::ValueOpcode::BufferAtomicUMax32:
		case IR::ValueOpcode::BufferAtomicUMax64:
		case IR::ValueOpcode::SharedAtomicUMax32: return spv::OpAtomicUMax;
		case IR::ValueOpcode::BufferAtomicAnd32:
		case IR::ValueOpcode::BufferAtomicAnd64:
		case IR::ValueOpcode::SharedAtomicAnd32: return spv::OpAtomicAnd;
		case IR::ValueOpcode::BufferAtomicOr32:
		case IR::ValueOpcode::BufferAtomicOr64:
		case IR::ValueOpcode::SharedAtomicOr64:
		case IR::ValueOpcode::SharedAtomicOr32: return spv::OpAtomicOr;
		case IR::ValueOpcode::BufferAtomicXor32:
		case IR::ValueOpcode::BufferAtomicXor64:
		case IR::ValueOpcode::SharedAtomicXor32: return spv::OpAtomicXor;
		default: return spv::OpNop;
	}
}

uint32_t EmitAtomicOperation(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t pointer,
                             uint32_t scope) {
	const auto old = ctx.state.builder.AllocateId();
	if (inst.GetOpcode() == IR::ValueOpcode::BufferAtomicCmpSwap32) {
		const auto desired    = ctx.Arg(inst, inst.NumArgs() - 3);
		const auto comparator = ctx.Arg(inst, inst.NumArgs() - 2);
		ctx.state.builder.AddFunction(
		    spv::OpAtomicCompareExchange, TypeU32(ctx.state), old, pointer,
		    ConstantU32(ctx.state, scope), ConstantU32(ctx.state, spv::MemorySemanticsMaskNone),
		    ConstantU32(ctx.state, spv::MemorySemanticsMaskNone), desired, comparator);
	} else {
		const auto value = ctx.Arg(inst, inst.NumArgs() - 2);
		ctx.state.builder.AddFunction(SpirvAtomicOpcode(inst.GetOpcode()), TypeU32(ctx.state), old,
		                              pointer, ConstantU32(ctx.state, scope),
		                              ConstantU32(ctx.state, spv::MemorySemanticsMaskNone), value);
	}
	return old;
}

template <typename Fn>
uint32_t EmitAtomicAccess(ValueEmitContext& ctx, const IR::Inst& inst,
                          const IR::MemoryInfo& mem, Fn&& operation) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto access = PrepareMemoryElement(ctx, mem, DwordIndex(ctx, inst, mem));
		return EmitValueOrZeroIfCondition(
		    ctx.state, EmitMemoryElementInBounds(ctx.state, access.resource, access.index), [&]() {
			    return operation(EmitMemoryElementPointer(ctx.state, access.resource, access.index));
		    });
	});
}

template <typename Fn>
uint32_t EmitAtomicUpdate(ValueEmitContext& ctx, const IR::Inst& inst,
                          const IR::MemoryInfo& mem, Fn&& replacement) {
	const auto value = ctx.Arg(inst, inst.NumArgs() - 2);
	return EmitAtomicAccess(ctx, inst, mem, [&](uint32_t pointer) {
		return AtomicUpdate(ctx.state, pointer, mem.kind, [&](uint32_t old) {
			return replacement(ctx.state, old, value);
		});
	});
}

struct PreparedFormattedMemory {
	Format::BufferFormatInfo info;
	MemoryResourceAccess     resource;
	std::array<uint32_t, 4>  addresses {};
	std::array<uint32_t, 4>  indices {};
	uint32_t                 in_bounds = 0;
};

enum class FormattedAccess { Load, Store };

PreparedFormattedMemory PrepareFormattedMemory(ValueEmitContext& ctx, const IR::Inst& inst,
                                               const IR::MemoryInfo&       mem,
                                               const MemoryResourceAccess& resource,
                                               const Format::BufferFormatInfo& info,
                                               uint32_t components, FormattedAccess access) {
	PreparedFormattedMemory plan;
	plan.info     = info;
	plan.resource = resource;
	std::array<bool, 4> required_components {};
	if (access == FormattedAccess::Load) {
		for (uint32_t output = 0; output < components; output++) {
			const auto source = ResolveFormattedSource(ctx, mem, plan.info, output);
			if (source.kind == FormattedSourceKind::Memory) {
				required_components[source.component] = true;
			}
		}
	} else {
		for (uint32_t component = 0; component < std::min(components, plan.info.component_count);
		     component++) {
			required_components[component] = true;
		}
	}
	bool first_bound = true;
	for (uint32_t component = 0; component < plan.info.component_count; component++) {
		if (!required_components[component]) continue;
		const auto byte_offset = Format::GetFormatComponentByteOffset(plan.info, component);
		bool       reused      = false;
		for (uint32_t previous = 0; previous < component; previous++) {
			if (required_components[previous] &&
			    Format::GetFormatComponentByteOffset(plan.info, previous) == byte_offset) {
				plan.addresses[component] = plan.addresses[previous];
				plan.indices[component]   = plan.indices[previous];
				reused                    = true;
				break;
			}
		}
		if (reused) continue;
		const auto component_mem  = RebaseFormattedComponent(mem, plan.info, component);
		plan.addresses[component] = ByteAddress(ctx, inst, component_mem);
		const auto raw_index      = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state),
		                                   plan.addresses[component], ConstantU32(ctx.state, 2));
		plan.indices[component]   = EmitMemoryElementIndex(ctx.state, resource, raw_index);
		const auto component_bound =
		    EmitMemoryElementInBounds(ctx.state, resource, plan.indices[component]);
		if (first_bound) {
			plan.in_bounds = component_bound;
			first_bound    = false;
		} else {
			plan.in_bounds = AndCondition(ctx.state, plan.in_bounds, component_bound);
		}
	}
	if (first_bound) plan.in_bounds = ConstantBool(ctx.state, true);
	return plan;
}

uint32_t LoadFormattedInBounds(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                               const PreparedFormattedMemory& plan, uint32_t output_component) {
	return LoadFormattedComponent(
	    ctx, mem, plan.info, output_component,
	    [&](uint32_t component) {
		    return LoadWordInBounds(ctx, plan.resource, plan.indices[component]);
	    },
	    [&](uint32_t component, uint32_t bits, bool sign_extend) {
		    return LoadSubwordInBounds(ctx, plan.resource, plan.addresses[component],
		                               plan.indices[component], bits, sign_extend);
	    });
}

uint32_t ConstructU32Composite(EmitterState& state, uint32_t components,
                               const std::array<uint32_t, 4>& values) {
	const auto            result = state.builder.AllocateId();
	std::vector<uint32_t> words {spv::OpCompositeConstruct, TypeU32Composite(state, components),
	                             result};
	words.insert(words.end(), values.begin(), values.begin() + components);
	state.builder.AddFunction(words);
	return result;
}

uint32_t FormattedOutOfBoundsValue(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                   const PreparedFormattedMemory& plan, uint32_t components) {
	std::array<uint32_t, 4> values {};
	for (uint32_t component = 0; component < components; component++) {
		const auto source = ResolveFormattedSource(ctx, mem, plan.info, component);
		values[component] = FormattedConstant(ctx, plan.info, source.kind);
	}
	return ConstructU32Composite(ctx.state, components, values);
}

void StoreFormattedInBounds(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                            const PreparedFormattedMemory& plan, uint32_t component,
                            uint32_t data) {
	if (component >= plan.info.component_count) return;
	const auto bits   = plan.info.component_bits[component];
	const auto packed = PackFormatComponent(ctx.state, plan.info, component, data);
	if (bits == 8u || bits == 16u) {
		StoreSubwordInBounds(ctx, mem, plan.resource, plan.addresses[component],
		                     plan.indices[component], bits, packed);
	} else {
		StoreWordInBounds(ctx, plan.resource, plan.indices[component], packed);
	}
}

bool IsD16IntegerFormat(const Format::BufferFormatInfo& info) {
	return info.type == Format::ComponentType::Uint || info.type == Format::ComponentType::Sint ||
	       info.type == Format::ComponentType::Unknown;
}

// Formatted D16 loads return each component as 16 bits: a half float for float, normalized and
// scaled formats and the low half of the integer otherwise.
uint32_t FormattedToD16(EmitterState& state, const Format::BufferFormatInfo& info,
                        uint32_t value) {
	if (IsD16IntegerFormat(info)) {
		return Binary(state, spv::OpBitwiseAnd, TypeU32(state), value, ConstantU32(state, 0xffffu));
	}
	const auto pair = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(state, 2), pair,
	                          Unary(state, spv::OpBitcast, TypeF32(state), value),
	                          ConstantF32Value(state, 0.0f));
	const auto packed = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeU32(state), packed, GlslStd450(state),
	                          GLSLstd450PackHalf2x16, pair);
	return packed;
}

uint32_t FormattedToD16(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t value,
                        uint32_t components) {
	auto&      state = ctx.state;
	const auto info  = Format::GetFormatInfo(BufferFormat(ctx, mem));
	if (components == 1u) {
		return FormattedToD16(state, info, value);
	}
	std::array<uint32_t, 4> values {};
	for (uint32_t component = 0; component < components; component++) {
		const auto extracted = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), extracted, value,
		                          component);
		values[component] = FormattedToD16(state, info, extracted);
	}
	return ConstructU32Composite(state, components, values);
}

// Formatted D16 stores widen each 16-bit component back to the 32-bit value the format packs.
uint32_t D16ToFormatted(EmitterState& state, const Format::BufferFormatInfo& info,
                        uint32_t value) {
	if (info.type == Format::ComponentType::Sint) {
		const auto extended = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpBitFieldSExtract, TypeI32(state), extended,
		                          Unary(state, spv::OpBitcast, TypeI32(state), value),
		                          ConstantU32(state, 0), ConstantU32(state, 16));
		return Unary(state, spv::OpBitcast, TypeU32(state), extended);
	}
	if (IsD16IntegerFormat(info)) {
		return value;
	}
	const auto unpacked = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeF32Vector(state, 2), unpacked,
	                          GlslStd450(state), GLSLstd450UnpackHalf2x16, value);
	const auto low = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), low, unpacked, 0u);
	return Unary(state, spv::OpBitcast, TypeU32(state), low);
}

void FormattedStore(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		const auto data     = ctx.Arg(inst, inst.NumArgs() - 2);
		const auto info     = Format::GetFormatInfo(BufferFormat(ctx, mem));
		if (info.type == Format::ComponentType::Unknown) {
			StoreWordPrepared(ctx, inst, RebaseRawComponent(mem, 0u), resource, data);
			return;
		}
		const auto plan = PrepareFormattedMemory(ctx, inst, mem, resource, info, 1u,
		                                         FormattedAccess::Store);
		EmitIfCondition(ctx.state, plan.in_bounds, [&]() {
			StoreFormattedInBounds(ctx, mem, plan, 0u,
			                       mem.d16 ? D16ToFormatted(ctx.state, info, data) : data);
		});
	});
}

// A raw (or 32-bit formatted) DWORD x1-x4 access through a V# the shader computed
// (ResourceKind::IndirectBuffer): each component's guest byte address and whether it is in range
// under the V#'s OOB_SELECT mode (RDNA2 ISA 8.1.5); an invalid FORMAT (0: unbound) is out of range.
struct IndirectBufferComponents {
	std::array<uint32_t, 4> guests {};     // U64 guest byte addresses
	std::array<uint32_t, 4> conditions {}; // bool, per component
	uint32_t                all_in_bounds = 0;
};

IndirectBufferComponents IndirectBufferAccess(ValueEmitContext& ctx, const IR::Inst& inst,
                                              uint32_t components) {
	auto&       state   = ctx.state;
	const auto& handle  = *inst.Arg(0).ResolveInstruction();
	const auto  word1   = ctx.Arg(handle, 1);
	const auto  records = ctx.Arg(handle, 2);
	const auto  word3   = ctx.Arg(handle, 3);
	const auto  field   = [&](uint32_t word, uint32_t first, uint32_t count) {
		return EmitBitFieldUExtract(state, word, ConstantU32(state, first),
		                            ConstantU32(state, count));
	};
	const auto nonzero = [&](uint32_t value) {
		return Binary(state, spv::OpINotEqual, TypeBool(state), value, ConstantU32(state, 0));
	};
	const auto has_dword = [&](uint32_t offset, uint32_t size) {
		return AndCondition(
		    state,
		    Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), size, ConstantU32(state, 4)),
		    Binary(state, spv::OpULessThanEqual, TypeBool(state), offset,
		           Binary(state, spv::OpISub, TypeU32(state), size, ConstantU32(state, 4))));
	};
	const auto stride       = field(word1, 16, 14);
	const auto swizzle      = AndCondition(state, nonzero(stride), nonzero(field(word1, 31, 1)));
	const auto index_stride = field(word3, 21, 2);
	const auto add_tid      = nonzero(field(word3, 23, 1));
	const auto index =
	    Binary(state, spv::OpIAdd, TypeU32(state), ctx.Arg(inst, 1),
	           Select(state, TypeU32(state), add_tid, BufferLane(state), ConstantU32(state, 0)));
	const auto soffset = ctx.Arg(inst, 3);
	const auto base    = DeviceAddressFromWords(state, ctx.Arg(handle, 0), field(word1, 0, 16));
	const auto format       = field(word3, 12, 7);
	auto       valid_format = nonzero(format);
	if (ctx.Memory(inst).formatted) {
		const auto wide_first  = components == 4u ? Prospero::BufferFormat::k32_32_32_32UInt
		                                          : Prospero::BufferFormat::k32_32_32UInt;
		const auto wide_format = AndCondition(
		    state,
		    Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), format,
		           ConstantU32(state, static_cast<uint32_t>(wide_first))),
		    Binary(state, spv::OpULessThanEqual, TypeBool(state), format,
		           ConstantU32(state,
		                       static_cast<uint32_t>(Prospero::BufferFormat::k32_32_32_32Float))));
		const auto two_word_format = AndCondition(
		    state,
		    Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), format,
		           ConstantU32(state, static_cast<uint32_t>(Prospero::BufferFormat::k32_32UInt))),
		    Binary(state, spv::OpULessThanEqual, TypeBool(state), format,
		           ConstantU32(state, static_cast<uint32_t>(Prospero::BufferFormat::k32_32Float))));
		const auto scalar_format = AndCondition(
		    state,
		    Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), format,
		           ConstantU32(state, static_cast<uint32_t>(Prospero::BufferFormat::k32UInt))),
		    Binary(state, spv::OpULessThanEqual, TypeBool(state), format,
		           ConstantU32(state, static_cast<uint32_t>(Prospero::BufferFormat::k32Float))));
		const auto in_range =
		    components == 1u ? Binary(state, spv::OpLogicalOr, TypeBool(state), scalar_format,
		                              Binary(state, spv::OpLogicalOr, TypeBool(state),
		                                     two_word_format, wide_format))
		    : components == 2u
		        ? Binary(state, spv::OpLogicalOr, TypeBool(state), two_word_format, wide_format)
		        : wide_format;
		const auto identity_xy = AndCondition(state,
		                                      Binary(state, spv::OpIEqual, TypeBool(state),
		                                             field(word3, 0, 3), ConstantU32(state, 4u)),
		                                      Binary(state, spv::OpIEqual, TypeBool(state),
		                                             field(word3, 3, 3), ConstantU32(state, 5u)));
		const auto identity_xyz =
		    components == 2u ? identity_xy
		                     : AndCondition(state, identity_xy,
		                                    Binary(state, spv::OpIEqual, TypeBool(state),
		                                           field(word3, 6, 3), ConstantU32(state, 6u)));
		const auto identity = components == 4u
		                          ? AndCondition(state, identity_xyz,
		                                         Binary(state, spv::OpIEqual, TypeBool(state),
		                                                field(word3, 9, 3), ConstantU32(state, 7u)))
		                          : identity_xyz;
		const auto selected_identity = components == 1u
		                                   ? Binary(state, spv::OpIEqual, TypeBool(state),
		                                            field(word3, 0, 3), ConstantU32(state, 4u))
		                                   : identity;
		valid_format                 = AndCondition(state, in_range, selected_identity);
	}
	const auto mode            = field(word3, 28, 2);
	const auto index_in_bounds = Binary(state, spv::OpULessThan, TypeBool(state), index, records);
	const auto scalar_in_bounds =
	    Binary(state, spv::OpULessThanEqual, TypeBool(state), soffset, records);
	const auto raw_records = Binary(state, spv::OpISub, TypeU32(state), records, soffset);
	const auto raw_index_in_bounds =
	    Binary(state, spv::OpULessThan, TypeBool(state), index, raw_records);
	std::array<uint32_t, 4> guests {}, conditions {};
	auto                    all_in_bounds = valid_format;
	for (uint32_t component = 0; component < components; component++) {
		const auto address = CalculateBufferAddress(state, index, ctx.Arg(inst, 2), soffset,
		                                            ctx.Memory(inst).offset + component * 4u,
		                                            stride, swizzle, index_stride);
		// RDNA2 raw DWORD vectors check each component, unlike formatted accesses.
		const auto structured_bounds =
		    AndCondition(state, index_in_bounds,
		                 Binary(state, spv::OpULessThan, TypeBool(state), address.offset, stride));
		// OOB_SELECT=3 reduces NUM_RECORDS by SOFFSET before the offset/index check.
		const auto raw_bounds = AndCondition(
		    state, scalar_in_bounds,
		    Select(state, TypeBool(state), swizzle,
		           AndCondition(state, raw_index_in_bounds, has_dword(address.offset, stride)),
		           has_dword(address.offset, raw_records)));
		auto in_bounds =
		    Select(state, TypeBool(state),
		           Binary(state, spv::OpIEqual, TypeBool(state), mode, ConstantU32(state, 0)),
		           structured_bounds, index_in_bounds);
		in_bounds = Select(
		    state, TypeBool(state),
		    Binary(state, spv::OpULessThan, TypeBool(state), mode, ConstantU32(state, 2)),
		    in_bounds,
		    Select(state, TypeBool(state),
		           Binary(state, spv::OpIEqual, TypeBool(state), mode, ConstantU32(state, 2)),
		           nonzero(records), raw_bounds));
		const auto guest =
		    Binary(state, spv::OpIAdd, TypeScalarU64(state), base,
		           Unary(state, spv::OpUConvert, TypeScalarU64(state), address.byte));
		guests[component]     = guest;
		conditions[component] = AndCondition(state, valid_format, in_bounds);
		all_in_bounds         = AndCondition(state, all_in_bounds, in_bounds);
	}
	return {.guests = guests, .conditions = conditions, .all_in_bounds = all_in_bounds};
}

uint32_t LoadIndirectBuffer(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	const auto              access = IndirectBufferAccess(ctx, inst, components);
	std::array<uint32_t, 4> values {};
	for (uint32_t component = 0; component < components; component++) {
		values[component] = LoadBda(
		    ctx, access.guests[component],
		    ctx.Memory(inst).formatted ? access.all_in_bounds : access.conditions[component], 32u);
	}
	return components == 1u ? values[0] : ConstructU32Composite(ctx.state, components, values);
}

// Sets the bit of caching page `page` in the written-page bitmap unless it is already set.
void RecordBdaWritePage(EmitterState& state, uint32_t page) {
	const auto word =
	    Binary(state, spv::OpIAdd, TypeU32(state),
	           Binary(state, spv::OpShiftRightLogical, TypeU32(state), page, ConstantU32(state, 5)),
	           ConstantU32(state, static_cast<uint32_t>(BufferCache::BDA_WRITE_BITMAP_WORD)));
	const auto bit =
	    Binary(state, spv::OpShiftLeftLogical, TypeU32(state), ConstantU32(state, 1),
	           Binary(state, spv::OpBitwiseAnd, TypeU32(state), page, ConstantU32(state, 31)));
	const auto pointer = FaultElementPointer(state, word);
	const auto current = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAtomicLoad, TypeU32(state), current, pointer,
	                          ConstantU32(state, spv::ScopeDevice),
	                          ConstantU32(state, spv::MemorySemanticsMaskNone));
	const auto missing =
	    Binary(state, spv::OpIEqual, TypeBool(state),
	           Binary(state, spv::OpBitwiseAnd, TypeU32(state), current, bit), ConstantU32(state, 0));
	EmitIfCondition(state, missing, [&]() {
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAtomicOr, TypeU32(state), result, pointer,
		                          ConstantU32(state, spv::ScopeDevice),
		                          ConstantU32(state, spv::MemorySemanticsMaskNone), bit);
	});
}

// KYTY_BDA_WRITES_SHADERS: marks the caching page a BDA write lands in, in the fault buffer's
// written-page bitmap (BufferCache::BDA_WRITE_BITMAP_WORD); the renderer settles the marked pages
// after the dispatch. When every active lane writes the same page (a wave's neighbouring records,
// the common case) one elected lane marks it, so the bitmap sees one atomic per wave instead of
// one per lane; a bit already set is only read.
void RecordBdaWrite(EmitterState& state, uint32_t guest) {
	state.builder.RequireCapability(spv::CapabilityGroupNonUniform);
	state.builder.RequireCapability(spv::CapabilityGroupNonUniformVote);
	const auto page = Unary(state, spv::OpUConvert, TypeU32(state),
	                        Binary(state, spv::OpShiftRightLogical, TypeScalarU64(state), guest,
	                               ConstantDeviceAddress(state, BufferCache::CACHING_PAGEBITS)));
	const auto uniform = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformAllEqual, TypeBool(state), uniform,
	                          ConstantU32(state, spv::ScopeSubgroup), page);
	const auto elected = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformElect, TypeBool(state), elected,
	                          ConstantU32(state, spv::ScopeSubgroup));
	const auto marks = Binary(state, spv::OpLogicalOr, TypeBool(state),
	                          Unary(state, spv::OpLogicalNot, TypeBool(state), uniform), elected);
	EmitIfCondition(state, marks, [&]() { RecordBdaWritePage(state, page); });
}

// KYTY_BDA_WRITES_SHADERS: a write to a page without a cache buffer cannot land; get_bda_pointer
// already recorded the page fault (a later dispatch finds a buffer there), and the settle counts it.
void RecordBdaDroppedWrite(EmitterState& state) {
	const auto pointer = FaultElementPointer(
	    state, ConstantU32(state, static_cast<uint32_t>(BufferCache::BDA_DROPPED_WRITES_WORD)));
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAtomicIAdd, TypeU32(state), result, pointer,
	                          ConstantU32(state, spv::ScopeDevice),
	                          ConstantU32(state, spv::MemorySemanticsMaskNone),
	                          ConstantU32(state, 1));
}

// KYTY_BDA_WRITES_SHADERS: a raw BUFFER_STORE_DWORD[X2-X4] through a V# the shader computed, written
// through BDA. Each DWORD is range-checked on its own (RDNA2 ISA 8.1.5) and its address ignores the
// two LSBs (8.1.7); out-of-range components are not written. The written page is marked for the
// renderer's settle before the store, and a page without a cache buffer drops the write.
void StoreIndirectBuffer(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	EmitIfCondition(state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto access    = IndirectBufferAccess(ctx, inst, components);
		const auto composite = ctx.Arg(inst, inst.NumArgs() - 2);
		for (uint32_t component = 0; component < components; component++) {
			auto data = composite;
			if (components > 1u) {
				data = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), data, composite,
				                          component);
			}
			EmitIfCondition(state, access.conditions[component], [&]() {
				const auto guest =
				    Binary(state, spv::OpBitwiseAnd, TypeScalarU64(state), access.guests[component],
				           ConstantDeviceAddress(state, ~uint64_t {3}));
				const auto host    = GetBdaPointer(ctx, guest);
				const auto present = Binary(state, spv::OpINotEqual, TypeBool(state), host,
				                            ConstantDeviceAddress(state, 0));
				// KYTY_BDA_WRITES=candidates: no written-page bitmap (the destinations were claimed
				// before the dispatch), but a dropped write is still counted, and reported without a
				// settle (FaultManager::QueueBdaDroppedCheck).
				const bool candidates = BdaWriteCandidatesApplies(state.program.shader_hash) &&
				                        !BdaWriteCandidatesVerify();
				EmitIfCondition(state, Unary(state, spv::OpLogicalNot, TypeBool(state), present),
				                [&]() { RecordBdaDroppedWrite(state); });
				EmitIfCondition(state, present, [&]() {
					if (!candidates) RecordBdaWrite(state, guest);
					const auto pointer = state.builder.AllocateId();
					state.builder.AddFunction(spv::OpConvertUToPtr, TypePhysicalU32Pointer(state),
					                          pointer, host);
					constexpr uint32_t alignment = sizeof(uint32_t);
					state.builder.AddFunction(spv::OpStore, pointer, data,
					                          spv::MemoryAccessAlignedMask, alignment);
				});
			});
		}
	});
}

uint32_t LoadWideBuffer(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1),
	    components == 1u ? TypeU32(state) : TypeU32Composite(state, components),
	    components == 1u ? ConstantU32(state, 0) : ConstantU32CompositeZero(state, components),
	    [&]() {
		    const auto mem = ctx.Memory(inst);
		    if (mem.kind == IR::ResourceKind::IndirectBuffer) {
			    return LoadIndirectBuffer(ctx, inst, components);
		    }
		    const auto resource = PrepareMemoryResourceAccess(state, mem);
		    const auto info = Format::GetFormatInfo(
		        mem.formatted ? BufferFormat(ctx, mem) : Prospero::BufferFormat::kInvalid);
		    if (info.type != Format::ComponentType::Unknown) {
			    const auto plan = PrepareFormattedMemory(ctx, inst, mem, resource, info, components,
			                                             FormattedAccess::Load);
			    return EmitValueOrDefaultIfCondition(
			        state, plan.in_bounds, TypeU32Composite(state, components),
			        FormattedOutOfBoundsValue(ctx, mem, plan, components), [&]() {
				        std::array<uint32_t, 4> values {};
				        for (uint32_t component = 0; component < components; component++) {
					        values[component] = LoadFormattedInBounds(ctx, mem, plan, component);
				        }
				        return ConstructU32Composite(state, components, values);
			        });
		    }
		    std::array<uint32_t, 4> values {};
		    for (uint32_t component = 0; component < components; component++) {
			    values[component] =
			        LoadWordPrepared(ctx, inst, RebaseRawComponent(mem, component), resource);
		    }
		    return ConstructU32Composite(state, components, values);
	    });
}

void StoreWideBuffer(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	EmitIfCondition(state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto mem       = ctx.Memory(inst);
		const auto resource  = PrepareMemoryResourceAccess(state, mem);
		const auto composite = ctx.Arg(inst, inst.NumArgs() - 2);
		const auto info = Format::GetFormatInfo(
		    mem.formatted ? BufferFormat(ctx, mem) : Prospero::BufferFormat::kInvalid);
		if (info.type != Format::ComponentType::Unknown) {
			const auto plan = PrepareFormattedMemory(ctx, inst, mem, resource, info, components,
			                                         FormattedAccess::Store);
			EmitIfCondition(state, plan.in_bounds, [&]() {
				const auto count = std::min(components, plan.info.component_count);
				uint32_t   word  = 0;
				for (uint32_t component = 0; component < count; component++) {
					const auto extracted = state.builder.AllocateId();
					state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), extracted,
					                          composite, component);
					const auto data =
					    mem.d16 ? D16ToFormatted(state, plan.info, extracted) : extracted;
					if (!plan.info.packed_bitfield) {
						StoreFormattedInBounds(ctx, mem, plan, component, data);
						continue;
					}
					const auto packed  = PackFormatComponent(state, plan.info, component, data);
					const auto shifted = Binary(
					    state, spv::OpShiftLeftLogical, TypeU32(state), packed,
					    ConstantU32(state, plan.info.component_bit_offset[component]));
					word = component == 0u
					           ? shifted
					           : Binary(state, spv::OpBitwiseOr, TypeU32(state), word, shifted);
				}
				if (plan.info.packed_bitfield) {
					StoreWordInBounds(ctx, plan.resource, plan.indices[0], word);
				}
			});
			return;
		}
		for (uint32_t component = 0; component < components; component++) {
			const auto data = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), data, composite,
			                          component);
			StoreWordPrepared(ctx, inst, RebaseRawComponent(mem, component), resource, data);
		}
	});
}

uint32_t LoadWideShared(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1), TypeU32Composite(state, components),
	    ConstantU32CompositeZero(state, components), [&]() {
		    const auto              mem      = ctx.Memory(inst);
		    const auto              resource = PrepareMemoryResourceAccess(state, mem);
		    const auto              base     = ByteAddress(ctx, inst, mem);
		    std::array<uint32_t, 4> values {};
		    for (uint32_t component = 0; component < components; component++) {
			    const auto address   = component == 0u
			                               ? base
			                               : Binary(state, spv::OpIAdd, TypeU32(state), base,
			                                        ConstantU32(state, component * 4u));
			    const auto raw_index = Binary(state, spv::OpShiftRightLogical, TypeU32(state),
			                                  address, ConstantU32(state, 2));
			    const auto index     = EmitMemoryElementIndex(state, resource, raw_index);
			    values[component]    = EmitValueOrZeroIfCondition(
			        state, EmitMemoryElementInBounds(state, resource, index),
			        [&]() { return LoadWordInBounds(ctx, resource, index); });
		    }
		    return ConstructU32Composite(state, components, values);
	    });
}

void StoreWideShared(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	EmitIfCondition(state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto mem      = ctx.Memory(inst);
		const auto resource = PrepareMemoryResourceAccess(state, mem);
		const auto base     = ByteAddress(ctx, inst, mem);
		for (uint32_t component = 0; component < components; component++) {
			const auto address = component == 0u ? base
			                                     : Binary(state, spv::OpIAdd, TypeU32(state), base,
			                                              ConstantU32(state, component * 4u));
			const auto raw_index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), address,
			                              ConstantU32(state, 2));
			const auto index = EmitMemoryElementIndex(state, resource, raw_index);
			EmitIfCondition(state, EmitMemoryElementInBounds(state, resource, index),
			                [&]() {
				                StoreWordInBounds(ctx, resource, index, ctx.Arg(inst, component + 1u));
			                });
		}
	});
}

} // namespace

uint32_t GetBdaPointer(EmitterState& state, uint32_t address) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunctionCall, TypeScalarU64(state), result,
	                          state.bda_pointer_function, address);
	return result;
}

void EmitShaderTrap(EmitterState& state, uint32_t pc, uint32_t code) {
	constexpr auto record_word = BufferCache::CACHING_NUMPAGES / 8 / sizeof(uint32_t);
	const auto     pointer     = FaultElementPointer(state, ConstantU32(state, record_word));
	const auto     old         = state.builder.AllocateId();
	// Only the winning invocation writes the payload. Queue completion, rather
	// than another shader invocation, publishes those writes to the host.
	state.builder.AddFunction(spv::OpAtomicCompareExchange, TypeU32(state), old, pointer,
	                          ConstantU32(state, spv::ScopeDevice), ConstantU32(state, 0),
	                          ConstantU32(state, 0), ConstantU32(state, 1), ConstantU32(state, 0));
	const auto won = Binary(state, spv::OpIEqual, TypeBool(state), old, ConstantU32(state, 0));
	EmitIfCondition(state, won, [&]() {
		const auto store = [&](size_t offset, uint32_t value) {
			state.builder.AddFunction(
			    spv::OpStore,
			    FaultElementPointer(state,
			                        ConstantU32(state, record_word + offset / sizeof(uint32_t))),
			    value);
		};
		store(offsetof(ShaderTrapRecord, shader_hash_low),
		      ConstantU32(state, static_cast<uint32_t>(state.program.shader_hash)));
		store(offsetof(ShaderTrapRecord, shader_hash_high),
		      ConstantU32(state, static_cast<uint32_t>(state.program.shader_hash >> 32)));
		store(offsetof(ShaderTrapRecord, pc), pc);
		store(offsetof(ShaderTrapRecord, code), code);
	});
}

namespace {

// Share the exact guarded/unaligned body, including the wide-load slow path.
void DefineScalarBdaLoads(EmitterState& state) {
	std::array<bool, 3> widths {};
	// Both indirect buffer emitters use the u32 helper. Keep it conservatively
	// whenever that domain is present, including dynamically selected descriptors.
	for (const auto& mem : state.program.memory_info) {
		if (mem.kind == IR::ResourceKind::IndirectBuffer) widths[2] = true;
	}
	for (const auto* block : state.program.blocks) {
		for (const auto& inst : *block) {
			const auto op = inst.GetOpcode();
			const auto address = IR::AddressOpcodeInfoOf(op);
			if (address.access != IR::AddressAccess::Read) continue;
			const auto& mem = state.program.memory_info.at(inst.Flags<IR::MemoryFlags>().index);
			if (op == IR::ValueOpcode::LoadAddressU32 && mem.planning_only) continue;
			// ScalarAddress uses an aligned dword load directly; scratch uses its
			// own storage. Global wide loads still need u32 for their slow path.
			if (mem.kind == IR::ResourceKind::ScalarAddress ||
			    mem.kind == IR::ResourceKind::Scratch) continue;
			widths[address.data_bits == 8u ? 0 : address.data_bits == 16u ? 1 : 2] = true;
		}
	}
	const auto type = TypeU32(state);
	const auto wide = TypeScalarU64(state);
	const auto function_type = state.builder.Type(spv::OpTypeFunction, type, wide, TypeBool(state));
	for (uint32_t index = 0; index < widths.size(); ++index) {
		if (!widths[index]) continue;
		const auto bits = 8u << index;
		const auto function = state.builder.AllocateId();
		state.bda_scalar_load_functions[index] = function;
		state.builder.AddName(function, fmt::format("load_bda_u{}", bits).c_str());
		state.builder.AddFunction(spv::OpFunction, type, function,
		                          spv::FunctionControlMaskNone, function_type);
		const auto address = state.builder.AllocateId();
		const auto active = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpFunctionParameter, wide, address);
		state.builder.AddFunction(spv::OpFunctionParameter, TypeBool(state), active);
		EmitLabel(state, state.builder.AllocateId());
		ValueEmitContext ctx(state);
		const auto value = LoadBdaInline(ctx, address, active, bits);
		state.builder.AddFunction(spv::OpReturnValue, value);
		state.builder.AddFunction(spv::OpFunctionEnd);
	}
}

// One body per width keeps the page-crossing fallback out of every load site.
// The driver can inline this function while retaining one translation on the common path.
void DefineWideBdaLoads(EmitterState& state) {
	std::array<bool, 3> widths {};
	for (const auto* block : state.program.blocks) {
		for (const auto& inst : *block) {
			const auto address = IR::AddressOpcodeInfoOf(inst.GetOpcode());
			if (address.access != IR::AddressAccess::Read || address.data_dwords <= 1u) continue;
			const auto& mem = state.program.memory_info.at(inst.Flags<IR::MemoryFlags>().index);
			if (mem.kind != IR::ResourceKind::Global) continue;
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::LoadAddressU32x2: widths[0] = true; break;
				case IR::ValueOpcode::LoadAddressU32x3: widths[1] = true; break;
				case IR::ValueOpcode::LoadAddressU32x4: widths[2] = true; break;
				default: break;
			}
		}
	}
	for (uint32_t count = 2; count <= 4; ++count) {
		if (!widths[count - 2]) continue;
		const auto type = TypeU32Composite(state, count);
		const auto wide = TypeScalarU64(state);
		const auto zero = state.builder.Constant(spv::OpConstantNull, type);
		const auto function_type = state.builder.Type(spv::OpTypeFunction, type, wide, TypeBool(state));
		const auto function = state.builder.AllocateId();
		state.bda_wide_load_functions[count - 2] = function;
		state.builder.AddName(function, fmt::format("load_bda_u32x{}", count).c_str());
		state.builder.AddFunction(spv::OpFunction, type, function,
		                          spv::FunctionControlMaskNone, function_type);
		const auto address = state.builder.AllocateId();
		const auto active = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpFunctionParameter, wide, address);
		state.builder.AddFunction(spv::OpFunctionParameter, TypeBool(state), active);
		EmitLabel(state, state.builder.AllocateId());
		ValueEmitContext ctx(state);
		const auto value = EmitValueOrDefaultIfCondition(state, active, type, zero, [&]() {
			const auto offset = Binary(state, spv::OpBitwiseAnd, wide, address,
			                           ConstantDeviceAddress(state, BufferCache::CACHING_PAGESIZE - 1));
			const auto aligned = Binary(state, spv::OpIEqual, TypeBool(state),
			    Binary(state, spv::OpBitwiseAnd, wide, address, ConstantDeviceAddress(state, 3)),
			    ConstantDeviceAddress(state, 0));
			const auto same_page = Binary(state, spv::OpULessThanEqual, TypeBool(state), offset,
			    ConstantDeviceAddress(state, BufferCache::CACHING_PAGESIZE - count * 4u));
			const auto fast = AndCondition(state, aligned, same_page);
			const auto fast_label = state.builder.AllocateId();
			const auto slow_label = state.builder.AllocateId();
			const auto merge = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpSelectionMerge, merge, spv::SelectionControlMaskNone);
			state.builder.AddFunction(spv::OpBranchConditional, fast, fast_label, slow_label);
			EmitLabel(state, fast_label);
			const auto bda = GetBdaPointer(ctx, address);
			const auto present = Binary(state, spv::OpINotEqual, TypeBool(state), bda,
			                            ConstantDeviceAddress(state, 0));
			const auto fast_value = EmitValueOrDefaultIfCondition(state, present, type, zero, [&]() {
				const auto vector_type = TypeU32Vector(state, count);
				const auto pointer_type = state.builder.Type(spv::OpTypePointer,
				    spv::StorageClassPhysicalStorageBuffer, vector_type);
				const auto pointer = Unary(state, spv::OpConvertUToPtr, pointer_type, bda);
				const auto loaded = state.builder.AllocateId();
				// Exactly N dwords, including x3: never overread the next page.
				state.builder.AddFunction(spv::OpLoad, vector_type, loaded, pointer,
				                          spv::MemoryAccessAlignedMask, 4u);
				// U32x2 is represented as a logical pair in IR; physical memory uses a vector.
				if (count == 2) {
					const auto first = state.builder.AllocateId();
					const auto second = state.builder.AllocateId();
					const auto pair = state.builder.AllocateId();
					state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), first, loaded, 0u);
					state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), second, loaded, 1u);
					state.builder.AddFunction(spv::OpCompositeConstruct, type, pair, first, second);
					return pair;
				}
				return loaded;
			});
			const auto fast_exit = state.current_label;
			state.builder.AddFunction(spv::OpBranch, merge);
			EmitLabel(state, slow_label);
			std::array<uint32_t, 4> components {};
			for (uint32_t i = 0; i < count; ++i) {
				const auto guest = i == 0 ? address : Binary(state, spv::OpIAdd, wide, address,
				                                             ConstantDeviceAddress(state, i * 4u));
				components[i] = LoadBda(ctx, guest, ConstantBool(state, true), 32u);
			}
			const auto slow_value = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeConstruct, type, slow_value,
			                          std::span(components).first(count));
			const auto slow_exit = state.current_label;
			state.builder.AddFunction(spv::OpBranch, merge);
			EmitLabel(state, merge);
			const auto merged = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpPhi, type, merged, fast_value, fast_exit,
			                          slow_value, slow_exit);
			return merged;
		});
		state.builder.AddFunction(spv::OpReturnValue, value);
		state.builder.AddFunction(spv::OpFunctionEnd);
	}
}

} // namespace

void DefineGetBdaPointer(EmitterState& state) {
	if (!state.program.info.uses_dma) {
		return;
	}
	const auto type            = TypeScalarU64(state);
	const auto function_type   = state.builder.Type(spv::OpTypeFunction, type, type);
	state.bda_pointer_function = state.builder.AllocateId();
	const auto address         = state.builder.AllocateId();
	const auto entry_label     = state.builder.AllocateId();
	state.builder.AddName(state.bda_pointer_function, "get_bda_pointer");
	state.builder.AddFunction(spv::OpFunction, type, state.bda_pointer_function,
	                          spv::FunctionControlMaskNone, function_type);
	state.builder.AddFunction(spv::OpFunctionParameter, type, address);
	EmitLabel(state, entry_label);

	// Reject noncanonical FLAT addresses before indexing the local 40-bit page table.
	const auto in_aperture = Binary(state, spv::OpULessThan, TypeBool(state), address,
	    ConstantDeviceAddress(state, BufferCache::CACHING_NUMPAGES * BufferCache::CACHING_PAGESIZE));
	const auto mapped_address = EmitValueOrDefaultIfCondition(
	    state, in_aperture, type, ConstantDeviceAddress(state, 0), [&]() {
	const auto page64        = Binary(state, spv::OpShiftRightLogical, type, address,
	                                  ConstantDeviceAddress(state, BufferCache::CACHING_PAGEBITS));
	const auto page          = Unary(state, spv::OpUConvert, TypeU32(state), page64);
	const auto entry_pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferU64ElementPointer(state),
	                          entry_pointer, state.bda_pagetable_variable, ConstantU32(state, 0),
	                          page);
	const auto base = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, type, base, entry_pointer);
	const auto missing =
	    Binary(state, spv::OpIEqual, TypeBool(state), base, ConstantDeviceAddress(state, 0));
	const auto fault_label     = state.builder.AllocateId();
	const auto available_label = state.builder.AllocateId();
	const auto merge_label     = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpSelectionMerge, merge_label, spv::SelectionControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, missing, fault_label, available_label);

	EmitLabel(state, fault_label);
	RecordBdaFault(state, page);
	state.builder.AddFunction(spv::OpBranch, merge_label);

	EmitLabel(state, available_label);
	const auto offset    = Binary(state, spv::OpBitwiseAnd, type, address,
	                              ConstantDeviceAddress(state, BufferCache::CACHING_PAGESIZE - 1));
	const auto available = Binary(state, spv::OpIAdd, type, base, offset);
	state.builder.AddFunction(spv::OpBranch, merge_label);

	EmitLabel(state, merge_label);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpPhi, type, result, ConstantDeviceAddress(state, 0),
	                          fault_label, available, available_label);
	return result;
	});
	state.builder.AddFunction(spv::OpReturnValue, mapped_address);
	state.builder.AddFunction(spv::OpFunctionEnd);
	DefineScalarBdaLoads(state);
	DefineWideBdaLoads(state);
}

uint32_t EmitAtomic32(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem = ctx.Memory(inst);
	return EmitAtomicAccess(ctx, inst, mem, [&](uint32_t pointer) {
		const auto scope =
		    mem.kind == IR::ResourceKind::Lds ? spv::ScopeWorkgroup : spv::ScopeDevice;
		const auto old = EmitAtomicOperation(ctx, inst, pointer, scope);
		if (mem.kind == IR::ResourceKind::Lds) {
			const auto semantics =
			    spv::MemorySemanticsAcquireReleaseMask | spv::MemorySemanticsWorkgroupMemoryMask;
			ctx.state.builder.AddFunction(spv::OpMemoryBarrier, ConstantU32(ctx.state, scope),
			                              ConstantU32(ctx.state, semantics));
		} else {
			EmitDeviceAtomicMemoryBarrier(ctx.state);
		}
		return old;
	});
}

uint32_t EmitBufferAtomic64(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem   = ctx.Memory(inst);
	auto&       state = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1), TypeU64(state), ConstantU64(state, 0), [&]() {
		    const auto resource = PrepareStorageBufferResourceAccess(
		        state, mem, state.storage_buffer_u64_variable, TypeStorageBufferU64Pointer(state));
		    const auto byte_address = Binary(state, spv::OpIAdd, TypeU32(state),
		                                     ByteAddress(ctx, inst, mem), resource.byte_offset);
		    const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), byte_address,
		                              ConstantU32(state, 3u));
		    return EmitValueOrDefaultIfCondition(
		        state, EmitMemoryElementInBounds(state, resource, index), TypeU64(state),
		        ConstantU64(state, 0), [&]() {
			        const auto value = Unary(state, spv::OpBitcast, TypeScalarU64(state),
			                                 ctx.Arg(inst, inst.NumArgs() - 2));
			        const auto old   = state.builder.AllocateId();
			        const auto pointer = EmitStorageBufferElementPointer(
			            state, resource, index, TypeStorageBufferU64ElementPointer(state));
			        if (inst.GetOpcode() == IR::ValueOpcode::BufferAtomicCmpSwap64) {
				        // DATA[0:1] is stored when memory equals the comparator in DATA[2:3].
				        const auto desired = Unary(state, spv::OpBitcast, TypeScalarU64(state),
				                                   ctx.Arg(inst, inst.NumArgs() - 3));
				        state.builder.AddFunction(
				            spv::OpAtomicCompareExchange, TypeScalarU64(state), old, pointer,
				            ConstantU32(state, spv::ScopeDevice),
				            ConstantU32(state, spv::MemorySemanticsMaskNone),
				            ConstantU32(state, spv::MemorySemanticsMaskNone), desired, value);
			        } else {
				        state.builder.AddFunction(SpirvAtomicOpcode(inst.GetOpcode()),
				                                  TypeScalarU64(state), old, pointer,
				                                  ConstantU32(state, spv::ScopeDevice),
				                                  ConstantU32(state, spv::MemorySemanticsMaskNone),
				                                  value);
			        }
			        EmitDeviceAtomicMemoryBarrier(state);
			        return Unary(state, spv::OpBitcast, TypeU64(state), old);
		        });
	    });
}

void EmitSharedAtomic64(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	const auto& mem = ctx.Memory(inst);
	EnsureLdsStorage(state);
	EmitIfCondition(state, ctx.Arg(inst, 2), [&]() {
		const auto address = Binary(state, spv::OpBitwiseAnd, TypeU32(state),
		                            ByteAddress(ctx, inst, mem), ConstantU32(state, 0xfff8u));
		const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), address,
		                          ConstantU32(state, 3u));
		const auto in_bounds = Binary(state, spv::OpULessThan, TypeBool(state), index,
		                              ConstantU32(state, LdsDwordCount(state) / 2u));
		EmitIfCondition(state, in_bounds, [&]() {
			const auto pointer = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpAccessChain,
			                          TypePointer(state, spv::StorageClassWorkgroup, TypeScalarU64(state)),
			                          pointer, state.lds_u64_variable, ConstantU32(state, 0), index);
			const auto value = Unary(state, spv::OpBitcast, TypeScalarU64(state), ctx.Arg(inst, 1));
			state.builder.AddFunction(
			    SpirvAtomicOpcode(inst.GetOpcode()), TypeScalarU64(state), state.builder.AllocateId(),
			    pointer, ConstantU32(state, spv::ScopeWorkgroup),
			    ConstantU32(state, spv::MemorySemanticsAcquireReleaseMask |
			                           spv::MemorySemanticsWorkgroupMemoryMask), value);
		});
	});
}

uint32_t EmitBufferFloatAtomic(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem       = ctx.Memory(inst);
	const bool  max_value = inst.GetOpcode() == IR::ValueOpcode::BufferAtomicFMax32;
	return EmitAtomicUpdate(ctx, inst, mem,
	                        [max_value](EmitterState& state, uint32_t old, uint32_t value) {
		                        return EmitFloatAtomicReplacement(state, old, value, max_value);
	                        });
}

void EmitSharedFloatAtomic(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem       = ctx.Memory(inst);
	const bool  max_value = inst.GetOpcode() == IR::ValueOpcode::SharedAtomicFMax32;
	EmitAtomicUpdate(ctx, inst, mem,
	                 [max_value](EmitterState& state, uint32_t old, uint32_t value) {
		                 return EmitDsFloatAtomicReplacement(state, old, value, max_value);
	                 });
}

uint32_t EmitAppendConsume(ValueEmitContext& ctx, const IR::Inst& inst) {
	const bool append = inst.GetOpcode() == IR::ValueOpcode::DataAppend;
	auto&      state  = ctx.state;
	if (ctx.half == 1) {
		return ctx.other_half->Def(IR::Value(const_cast<IR::Inst*>(&inst)));
	}
	const auto m0 = ctx.Arg(inst, 0);
	const auto base =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), m0, ConstantU32(state, 16));
	const auto size =
	    Binary(state, spv::OpBitwiseAnd, TypeU32(state), m0, ConstantU32(state, 0xffffu));
	const auto address = Binary(state, spv::OpIAdd, TypeU32(state), base,
	                            ConstantU32(state, ctx.Memory(inst).offset));
	const auto raw_index =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), address, ConstantU32(state, 2));
	const auto mem    = ctx.Memory(inst);
	const auto access = PrepareMemoryResourceAccess(state, mem);
	const auto index  = EmitMemoryElementIndex(state, access, raw_index);
	const auto exec   = ctx.Arg(inst, 1);
	const auto ballot = ctx.Ballot(inst.Arg(1));
	const auto low    = state.builder.AllocateId();
	const auto high   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), low, ballot, 0);
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), high, ballot, 1);
	// A wave32 in a 64-lane host subgroup (WaveHalvesInHostSubgroup): each half is its own guest
	// wave and counts its own lanes.
	const auto count =
	    WaveHalvesInHostSubgroup(state)
	        ? Unary(state, spv::OpBitCount, TypeU32(state), EmitOwnWaveHalfWord(state, ballot))
	        : Binary(state, spv::OpIAdd, TypeU32(state),
	                 Unary(state, spv::OpBitCount, TypeU32(state), low),
	                 Unary(state, spv::OpBitCount, TypeU32(state), high));
	// KYTY_PS_APPEND_LIVE_ELECTION: a helper invocation's atomic has no effect and returns an
	// undefined value, so a pixel shader elects the first EXEC lane that is not a helper (the
	// count above still covers every EXEC lane). With no such lane nobody consumes the result.
	auto elected_ballot = ballot;
	auto lane_guard     = exec;
	if (state.program.stage == ShaderType::Pixel && state.helper_invocation_variable != 0 &&
	    state.lane_count == 1) {
		const auto live =
		    Binary(state, spv::OpLogicalAnd, TypeBool(state), exec,
		           Unary(state, spv::OpLogicalNot, TypeBool(state), EmitIsHelperInvocation(state)));
		elected_ballot = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpGroupNonUniformBallot, TypeU32Vector(state, 4),
		                          elected_ballot, ConstantU32(state, spv::ScopeSubgroup), live);
		const auto elected_low  = state.builder.AllocateId();
		const auto elected_high = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), elected_low,
		                          elected_ballot, 0);
		state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), elected_high,
		                          elected_ballot, 1);
		const auto any_live =
		    Binary(state, spv::OpINotEqual, TypeBool(state),
		           Binary(state, spv::OpBitwiseOr, TypeU32(state), elected_low, elected_high),
		           ConstantU32(state, 0));
		// FindLSB of an empty ballot is undefined: without a live lane nothing is added.
		lane_guard = AndCondition(state, exec, any_live);
	}
	const auto first = ctx.FirstLane(elected_ballot);
	const auto source_lane =
	    state.lane_count == 2
	        ? Binary(state, spv::OpBitwiseAnd, TypeU32(state), first, ConstantU32(state, 31))
	        : first;
	const auto is_first       = Binary(state, spv::OpIEqual, TypeBool(state),
	                                   EmitSubgroupLocalInvocationId(state), source_lane);
	const auto storage_bounds = EmitMemoryElementInBounds(state, access, index);
	const auto m0_bounds =
	    mem.kind == IR::ResourceKind::Gds
	        ? Binary(state, spv::OpINotEqual, TypeBool(state), size, ConstantU32(state, 0))
	        : Binary(state, spv::OpULessThan, TypeBool(state),
	                 ConstantU32(state, ctx.Memory(inst).offset + 3u), size);
	const auto condition = AndCondition(
	    state, is_first,
	    AndCondition(state,
	                 state.lane_count == 2 ? Binary(state, spv::OpINotEqual, TypeBool(state), count,
	                                                ConstantU32(state, 0))
	                                       : lane_guard,
	                 AndCondition(state, storage_bounds, m0_bounds)));
	const auto atomic = EmitValueOrZeroIfCondition(state, condition, [&]() {
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction(append ? spv::OpAtomicIAdd : spv::OpAtomicISub, TypeU32(state),
		                          value, EmitMemoryElementPointer(state, access, index),
		                          ConstantU32(state, mem.kind == IR::ResourceKind::Gds
		                                                 ? spv::ScopeDevice
		                                                 : spv::ScopeWorkgroup),
		                          ConstantU32(state, spv::MemorySemanticsMaskNone), count);
		return value;
	});
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, TypeU32(state), result,
	                          ConstantU32(state, spv::ScopeSubgroup), atomic, source_lane);
	return result;
}

uint32_t EmitReadConst(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	if (state.flattened_srt_variable == 0) {
		ctx.Fail(inst, "requires the flattened SRT descriptor");
	}
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
	                          state.flattened_srt_variable, ConstantU32(state, 0),
	                          ctx.Arg(inst, 1));
	return EmitNative<spv::OpLoad, IR::Type::U32>(state, pointer);
}

void ProbeScalarRead(ValueEmitContext& ctx, const IR::Inst& inst,
                     uint32_t guest, uint32_t size, uint32_t offset, uint32_t in_bounds) {
	auto& s = ctx.state;
	const auto& options = GetCodegenOptions();
	const auto pc = inst.Flags<IR::MemoryFlags>().pc;
	if (options.scalar_read_probe_shader == 0 ||
	    options.scalar_read_probe_shader != s.program.shader_hash ||
	    std::ranges::find(options.scalar_read_probe_pcs, pc) == options.scalar_read_probe_pcs.end()) return;
	// The probe deliberately changes faulty work, never repairs it. Its loop
	// exits must not strand a workgroup at a barrier or split a virtual guest wave.
	if (s.program.stage != ShaderType::Compute || s.lane_count != 1 ||
	    WaveHalvesInHostSubgroup(s) || s.program.dispatcher_fallback)
		ctx.Fail(inst, "scalar read probe requires structured native-wave compute");
	for (const auto* block: s.program.blocks)
		for (const auto& instruction: *block)
			if (instruction.GetOpcode() == IR::ValueOpcode::Barrier)
				ctx.Fail(inst, "scalar read probe cannot exit loops with a workgroup barrier");
	const auto u = TypeU32(s), b = TypeBool(s), wide = TypeScalarU64(s);
	const auto zero = ConstantDeviceAddress(s, 0);
	const auto bda = EmitValueOrDefaultIfCondition(s, in_bounds, wide, zero, [&] {
		return GetBdaPointer(ctx, guest);
	});
	const auto bad = Binary(s, spv::OpIEqual, b, bda, zero);
	EmitIfCondition(s, bad, [&] {
		// Returning here is invalid when the read is in a continue construct.
		// Latch the failure and vote at existing loop exits instead; no CFG edge
		// or fabricated sentinel is introduced in the continue construct.
		s.builder.AddFunction(spv::OpStore, s.scalar_read_probe_failed, ConstantBool(s, true));
		constexpr auto first = BufferCache::FAULT_BITMAP_BYTES / sizeof(uint32_t);
		const auto claim = FaultElementPointer(s, ConstantU32(s, first));
		const auto old = s.builder.AllocateId();
		s.builder.AddFunction(spv::OpAtomicCompareExchange, u, old, claim,
		    ConstantU32(s, spv::ScopeDevice), ConstantU32(s, 0), ConstantU32(s, 0),
		    ConstantU32(s, 1), ConstantU32(s, 0));
		EmitIfCondition(s, Binary(s, spv::OpIEqual, b, old, ConstantU32(s, 0)), [&] {
			const auto store = [&](size_t byte, uint32_t value) {
				s.builder.AddFunction(spv::OpStore,
				    FaultElementPointer(s, ConstantU32(s, first + byte / 4)), value);
			};
			const auto store64 = [&](size_t byte, uint32_t value) {
				store(byte, Unary(s, spv::OpUConvert, u, value));
				store(byte + 4, Unary(s, spv::OpUConvert, u,
				    Binary(s, spv::OpShiftRightLogical, wide, value, ConstantDeviceAddress(s, 32))));
			};
			store(offsetof(ShaderTrapRecord, shader_hash_low), ConstantU32(s, uint32_t(s.program.shader_hash)));
			store(offsetof(ShaderTrapRecord, shader_hash_high), ConstantU32(s, uint32_t(s.program.shader_hash >> 32)));
			store(offsetof(ShaderTrapRecord, pc), ConstantU32(s, pc));
			store(offsetof(ShaderTrapRecord, code), ConstantU32(s, ShaderTrapRecord::ScalarReadFailure));
			const auto& handle = *inst.Arg(0).ResolveInstruction();
			for (uint32_t i = 0; i < 4; ++i)
				store(offsetof(ShaderTrapRecord, descriptor) + i * 4, ctx.Arg(handle, i));
			store64(offsetof(ShaderTrapRecord, address_low), guest);
			store64(offsetof(ShaderTrapRecord, size_low), size);
			store64(offsetof(ShaderTrapRecord, bda_low), bda);
			store(offsetof(ShaderTrapRecord, offset), offset);
			const auto canonical = Binary(s, spv::OpULessThan, b, guest,
			    ConstantDeviceAddress(s, BufferCache::CACHING_NUMPAGES * BufferCache::CACHING_PAGESIZE));
			store(offsetof(ShaderTrapRecord, reason), Select(s, u, in_bounds,
			    Select(s, u, canonical, ConstantU32(s, 3), ConstantU32(s, 2)), ConstantU32(s, 1)));
		});
	});
}

// One dword of an S_BUFFER_LOAD through a V# the shader computed at runtime (KYTY_SRT_VARIANT_READS,
// ResourceKind::IndirectBuffer), read through BDA. RDNA2 ISA 7.2.1, "Reads using Buffer Constant":
// only base, stride and num_records are used; addr = (base + OFFSET + SOFFSET) & ~3. The bound is
// the one a bound scalar buffer gets (ShaderBufferResource::GetSize: stride 0 makes num_records a
// byte count, otherwise stride * num_records bytes), and a dword with any byte past it reads 0, as
// the robust storage-buffer load of a bound V# does.
uint32_t LoadIndirectScalarBuffer(ValueEmitContext& ctx, const IR::Inst& inst,
                                  const IR::MemoryInfo& mem) {
	auto&       state   = ctx.state;
	const auto& handle  = *inst.Arg(0).ResolveInstruction();
	const auto  word1   = ctx.Arg(handle, 1);
	const auto  records = ctx.Arg(handle, 2);
	const auto  field   = [&](uint32_t word, uint32_t first, uint32_t count) {
		return EmitBitFieldUExtract(state, word, ConstantU32(state, first),
		                            ConstantU32(state, count));
	};
	const auto u64 = [&](uint32_t value) {
		return Unary(state, spv::OpUConvert, TypeScalarU64(state), value);
	};
	const auto stride = field(word1, 16, 14);
	const auto base   = DeviceAddressFromWords(state, ctx.Arg(handle, 0), field(word1, 0, 16));
	const auto offset = Binary(state, spv::OpIAdd, TypeU32(state), ctx.Arg(inst, 1),
	                           ConstantU32(state, mem.offset));
	const auto size   = Select(state, TypeScalarU64(state),
	                           Binary(state, spv::OpIEqual, TypeBool(state), stride,
	                                  ConstantU32(state, 0)),
	                           u64(records),
	                           Binary(state, spv::OpIMul, TypeScalarU64(state), u64(stride),
	                                  u64(records)));
	const auto dword_end =
	    Binary(state, spv::OpIAdd, TypeScalarU64(state),
	           u64(Binary(state, spv::OpBitwiseAnd, TypeU32(state), offset, ConstantU32(state, ~3u))),
	           ConstantDeviceAddress(state, sizeof(uint32_t)));
	const auto in_bounds =
	    Binary(state, spv::OpULessThanEqual, TypeBool(state), dword_end, size);
	// The ISA drops the two LSBs of the sum, so the dword never straddles (no unaligned merge).
	const auto guest = Binary(state, spv::OpBitwiseAnd, TypeScalarU64(state),
	                          Binary(state, spv::OpIAdd, TypeScalarU64(state), base, u64(offset)),
	                          ConstantDeviceAddress(state, ~uint64_t {3}));
	ProbeScalarRead(ctx, inst, guest, size, offset, in_bounds);
	return LoadBda(ctx, guest, in_bounds, 32u);
}

void EmitReadConstBuffer(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto mem = ctx.Memory(inst);
	if (mem.planning_only) return;
	if (mem.kind == IR::ResourceKind::IndirectBuffer) {
		ctx.Define(inst, LoadIndirectScalarBuffer(ctx, inst, mem));
		return;
	}
	auto& state        = ctx.state;
	mem.kind           = IR::ResourceKind::ScalarBuffer;
	const auto address = Binary(state, spv::OpIAdd, TypeU32(state), ctx.Arg(inst, 1),
	                            ConstantU32(state, mem.offset));
	auto index =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), address, ConstantU32(state, 2));
	if (GetHostBufferRobustness().null_descriptor_for_short_ranges &&
	    static_cast<int32_t>(mem.offset) > 0) {
		// Preserve a positive scalar offset's carry past 4 GiB in the dword index. Add the
		// quotient and low-bit carry separately so even an unaligned immediate cannot wrap.
		index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), ctx.Arg(inst, 1),
		               ConstantU32(state, 2));
		if ((mem.offset & 3u) != 0u) {
			const auto low = Binary(state, spv::OpBitwiseAnd, TypeU32(state), ctx.Arg(inst, 1),
			                        ConstantU32(state, 3));
			const auto low_sum = Binary(state, spv::OpIAdd, TypeU32(state), low,
			                            ConstantU32(state, mem.offset & 3u));
			index = Binary(state, spv::OpIAdd, TypeU32(state), index,
			               Binary(state, spv::OpShiftRightLogical, TypeU32(state), low_sum,
			                      ConstantU32(state, 2)));
		}
		if (mem.offset >= 4u) {
			index = Binary(state, spv::OpIAdd, TypeU32(state), index,
			               ConstantU32(state, mem.offset >> 2u));
		}
	}
	const auto access    = PrepareMemoryResourceAccess(state, mem);
	const auto element   = EmitMemoryElementIndex(state, access, index);
	ctx.Define(inst, LoadOrZero(state, access, PlainLoadInBounds(state, access, element), [&]() {
		           return EmitNative<spv::OpLoad, IR::Type::U32>(
		               state, EmitMemoryElementPointer(state, access, element));
	           }));
}

void EmitLoadMemory(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto  op  = inst.GetOpcode();
	const auto& mem = ctx.Memory(inst);
	if (op == IR::ValueOpcode::LoadAddressU32 && mem.planning_only) return;
	const auto buffer_components = IR::BufferComponentCount(op);
	const auto shared_components = IR::SharedComponentCount(op);
	const auto address_info      = IR::AddressOpcodeInfoOf(op);
	uint32_t   value;
	if (mem.kind == IR::ResourceKind::IndirectBuffer || buffer_components > 1u)
		value = LoadWideBuffer(ctx, inst, buffer_components);
	else if (shared_components > 1u)
		value = LoadWideShared(ctx, inst, shared_components);
	else if (mem.kind == IR::ResourceKind::ScalarAddress)
		value = LoadBdaDword(ctx, GuestAddress(ctx, inst, mem));
	else if (address_info.access == IR::AddressAccess::Read &&
	         mem.kind == IR::ResourceKind::Global && mem.data_dwords > 1u) {
		const auto count = mem.data_dwords;
		EXIT_IF(count > 4u || ctx.state.bda_wide_load_functions[count - 2] == 0);
		value = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpFunctionCall, TypeU32Composite(ctx.state, count), value,
		    ctx.state.bda_wide_load_functions[count - 2], GuestAddress(ctx, inst, mem),
		    ctx.Arg(inst, inst.NumArgs() - 1));
	}
	else if (address_info.access == IR::AddressAccess::Read &&
	         mem.kind != IR::ResourceKind::Scratch)
		value = LoadBda(ctx, GuestAddress(ctx, inst, mem), ctx.Arg(inst, inst.NumArgs() - 1),
		                address_info.data_bits);
	else if (op == IR::ValueOpcode::LoadBufferU32 && mem.formatted)
		value = FormattedLoad(ctx, inst, mem);
	else if (inst.GetType() == IR::Type::U8)
		value = LoadSubword(ctx, inst, mem, 8, false);
	else if (inst.GetType() == IR::Type::U16)
		value = LoadSubword(ctx, inst, mem, 16, false);
	else
		value = LoadWord(ctx, inst, mem);
	if (mem.d16) value = FormattedToD16(ctx, mem, value, std::max(buffer_components, 1u));
	ctx.Define(inst, value);
}

void EmitStoreMemory(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto  op                = inst.GetOpcode();
	const auto& mem               = ctx.Memory(inst);
	const auto  buffer_components = IR::BufferComponentCount(op);
	const auto  shared_components = IR::SharedComponentCount(op);
	const auto  type              = inst.Arg(inst.NumArgs() - 2).GetType();
	if (mem.kind == IR::ResourceKind::IndirectBuffer)
		StoreIndirectBuffer(ctx, inst, std::max(buffer_components, 1u));
	else if (buffer_components > 1u)
		StoreWideBuffer(ctx, inst, buffer_components);
	else if (shared_components > 1u)
		StoreWideShared(ctx, inst, shared_components);
	else if (op == IR::ValueOpcode::StoreBufferU32 && mem.formatted)
		FormattedStore(ctx, inst, mem);
	else if (type == IR::Type::U8)
		StoreSubword(ctx, inst, mem, 8);
	else if (type == IR::Type::U16)
		StoreSubword(ctx, inst, mem, 16);
	else
		StoreWord(ctx, inst, mem);
}

uint32_t EmitAtomicIncDec(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto op          = inst.GetOpcode();
	const bool increment   = op == IR::ValueOpcode::SharedAtomicInc32 ||
	                       op == IR::ValueOpcode::BufferAtomicInc32;
	const auto replacement = increment ? AtomicIncrement : AtomicDecrement;
	return EmitAtomicUpdate(ctx, inst, ctx.Memory(inst), replacement);
}

void EmitSharedAtomicMaskedOr32(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto keep = Unary(ctx.state, spv::OpNot, TypeU32(ctx.state), ctx.Arg(inst, 1));
	EmitAtomicUpdate(ctx, inst, ctx.Memory(inst),
	                 [keep](EmitterState& state, uint32_t old, uint32_t value) {
		                 return Binary(state, spv::OpBitwiseOr, TypeU32(state),
		                               Binary(state, spv::OpBitwiseAnd, TypeU32(state), old, keep), value);
	                 });
}

uint32_t EmitSwizzleU32(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	state.builder.AddFunction(spv::OpStore, ctx.scratch_u32_variable, ctx.Arg(inst, 0));
	const auto source = EmitNative<spv::OpLoad, IR::Type::U32>(state, ctx.scratch_u32_variable);
	const auto target = EmitDsSwizzleTargetLane(state, EmitSubgroupLocalInvocationId(state),
	                                            inst.Arg(1).IsImmediate() ? inst.Arg(1).U32() : 0);
	return EmitDsMaskedLaneRead(state, source, target, ctx.Arg(inst, 2));
}

uint32_t EmitPermuteU32(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state   = ctx.state;
	auto       lane    = EmitSubgroupLocalInvocationId(state);
	if (state.lane_count == 2) {
		lane = Binary(state, spv::OpBitwiseAnd, TypeU32(state), lane, ConstantU32(state, 31));
	}
	const auto address = ctx.Arg(inst, 1);
	const auto word    = Binary(state, spv::OpShiftRightLogical, TypeU32(state), lane,
	                            ConstantU32(state, 5));
	const auto ballot_word = [&](uint32_t predicate) {
		const auto ballot = state.builder.AllocateId();
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpGroupNonUniformBallot, TypeU32Vector(state, 4), ballot,
		                          ConstantU32(state, spv::ScopeSubgroup), predicate);
		state.builder.AddFunction(spv::OpVectorExtractDynamic, TypeU32(state), result, ballot, word);
		return result;
	};
	// RDNA2 permutes independently within each 32-lane half. Intersect the source
	// address bit ballots to find this destination's enabled writers without LDS.
	auto writers = ballot_word(ctx.Arg(inst, 2));
	for (uint32_t bit = 0; bit < 5; ++bit) {
		const auto address_bit = Binary(state, spv::OpBitwiseAnd, TypeU32(state), address,
		                                ConstantU32(state, 1u << (bit + 2)));
		const auto mask = ballot_word(Binary(state, spv::OpINotEqual, TypeBool(state),
		                                      address_bit, ConstantU32(state, 0)));
		const auto lane_bit = Binary(state, spv::OpBitwiseAnd, TypeU32(state), lane,
		                             ConstantU32(state, 1u << bit));
		const auto selected = Select(
		    state, TypeU32(state),
		    Binary(state, spv::OpINotEqual, TypeBool(state), lane_bit, ConstantU32(state, 0)),
		    mask, Unary(state, spv::OpNot, TypeU32(state), mask));
		writers = Binary(state, spv::OpBitwiseAnd, TypeU32(state), writers, selected);
	}
	const auto active = Binary(state, spv::OpINotEqual, TypeBool(state), writers,
	                           ConstantU32(state, 0));
	const auto base = Binary(state, spv::OpBitwiseAnd, TypeU32(state), lane,
	                         ConstantU32(state, ~31u));
	const auto source = Select(state, TypeU32(state), active,
	                           Binary(state, spv::OpBitwiseOr, TypeU32(state), base,
	                                  EmitFindUMsb32(state, writers)), lane);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, TypeU32(state), result,
	                          ConstantU32(state, spv::ScopeSubgroup), ctx.Arg(inst, 0), source);
	return Select(state, TypeU32(state), active, result, ConstantU32(state, 0));
}

uint32_t EmitBpermuteU32(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state  = ctx.state;
	const auto source = ctx.Arg(inst, 0);
	const auto index  = Binary(state, spv::OpBitwiseAnd, TypeU32(state),
	                           Binary(state, spv::OpShiftRightLogical, TypeU32(state),
	                                  ctx.Arg(inst, 1), ConstantU32(state, 2)),
	                           ConstantU32(state, 31));
	const auto base   = Binary(state, spv::OpBitwiseAnd, TypeU32(state),
	                           EmitSubgroupLocalInvocationId(state), ConstantU32(state, ~31u));
	const auto target = Binary(state, spv::OpBitwiseOr, TypeU32(state), base, index);
	return EmitDsMaskedLaneRead(state, source, target, ctx.Arg(inst, 2));
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
