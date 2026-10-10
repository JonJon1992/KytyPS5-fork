#include "graphics/shader/recompiler/CodegenOptions.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace Libs::Graphics::ShaderRecompiler {
namespace {

// Unset or empty keeps the default; "0" disables; anything else enables.
bool EnvFlag(const char* name, bool default_value) {
	const auto* value = std::getenv(name);
	if (value == nullptr || value[0] == '\0') {
		return default_value;
	}
	return std::strcmp(value, "0") != 0;
}

// Comma-separated hex shader hashes appended to `hashes`; unset adds none.
void ParseHashList(const char* list, std::vector<uint64_t>& hashes) {
	std::string_view text(list != nullptr ? list : "");
	while (!text.empty()) {
		const auto comma = text.find(',');
		const auto token = std::string(text.substr(0, comma));
		if (!token.empty()) {
			hashes.push_back(std::strtoull(token.c_str(), nullptr, 16));
		}
		if (comma == std::string_view::npos) {
			break;
		}
		text.remove_prefix(comma + 1);
	}
}

CodegenOptions FromEnvironment() {
	CodegenOptions options;
	options.movrel_range = EnvFlag("KYTY_MOVREL_RANGE", options.movrel_range);
	options.movrel_known_zeros = EnvFlag("KYTY_MOVREL_KNOWN_ZEROS", options.movrel_known_zeros);
	options.movrel_switch      = EnvFlag("KYTY_MOVREL_SWITCH", options.movrel_switch);
	options.uniform_lane_reads = EnvFlag("KYTY_UNIFORM_LANE_READS", options.uniform_lane_reads);
	options.short_f32_helpers  = EnvFlag("KYTY_SHORT_F32_HELPERS", options.short_f32_helpers);
	options.fast_float_min_max = EnvFlag("KYTY_FAST_FMINMAX", options.fast_float_min_max);
	options.fast_pkrtz         = EnvFlag("KYTY_FAST_PKRTZ", options.fast_pkrtz);
	options.single_f2i_saturation =
	    EnvFlag("KYTY_SINGLE_F2I_SATURATION", options.single_f2i_saturation);
	options.lod_stats_gate = EnvFlag("KYTY_LOD_STATS_GATE", options.lod_stats_gate);
	options.robust_buffer_loads = EnvFlag("KYTY_ROBUST_BUFFER_LOADS", options.robust_buffer_loads);
	options.readonly_buffers    = EnvFlag("KYTY_READONLY_BUFFERS", options.readonly_buffers);
	options.readonly_buffer_bindings = EnvFlag("KYTY_READONLY_BUFFER_BINDINGS", options.readonly_buffer_bindings);
	options.interp_modes = EnvFlag("KYTY_INTERP_MODES", options.interp_modes);
	options.sample_offsets = EnvFlag("KYTY_SAMPLE_OFFSETS", options.sample_offsets);
	options.sample_lod_clamp = EnvFlag("KYTY_SAMPLE_LOD_CLAMP", options.sample_lod_clamp);
	options.host_ftz_inputs  = EnvFlag("KYTY_HOST_FTZ_INPUTS", options.host_ftz_inputs);
	options.exec_selects     = EnvFlag("KYTY_EXEC_SELECTS", options.exec_selects);
	options.ps_append_live_election =
	    EnvFlag("KYTY_PS_APPEND_LIVE_ELECTION", options.ps_append_live_election);
	if (const auto* mode = std::getenv("KYTY_PS_LIVE_EXEC"); mode != nullptr && mode[0] != '\0') {
		options.ps_live_exec = std::strcmp(mode, "all") == 0 ? PsLiveExec::All
		                       : std::strcmp(mode, "0") == 0 ? PsLiveExec::Off
		                                                     : PsLiveExec::AppendConsume;
	}
	if (const auto* budget = std::getenv("KYTY_LOOP_GUARD"); budget != nullptr) {
		options.loop_guard_budget = static_cast<uint32_t>(std::strtoul(budget, nullptr, 0));
	}
	ParseHashList(std::getenv("KYTY_LOOP_GUARD_SHADERS"), options.loop_guard_shaders);
	options.srt_variant_reads = EnvFlag("KYTY_SRT_VARIANT_READS", options.srt_variant_reads);
	options.bindless_strided_compute =
	    EnvFlag("KYTY_BINDLESS_STRIDED_COMPUTE", options.bindless_strided_compute);
	ParseHashList(std::getenv("KYTY_BINDLESS_STRIDED_COMPUTE_SHADERS"),
	              options.bindless_strided_compute_shaders);
	ParseHashList(std::getenv("KYTY_BDA_WRITES_SHADERS"), options.bda_writes_shaders);
	ParseHashList(std::getenv("KYTY_SRT_RUNTIME_DATA_READS"), options.srt_runtime_data_read_shaders);
	if (const auto* mode = std::getenv("KYTY_BDA_WRITES"); mode != nullptr) {
		options.bda_write_mode = std::strcmp(mode, "candidates") == 0 ? BdaWriteMode::Candidates
		    : std::strcmp(mode, "candidates-verify") == 0 ? BdaWriteMode::CandidatesVerify
		    : std::strcmp(mode, "deferred") == 0 ? BdaWriteMode::Deferred
		    : BdaWriteMode::Off;
	}
	options.runtime_buffer_stride =
	    EnvFlag("KYTY_RUNTIME_BUFFER_STRIDE", options.runtime_buffer_stride);
	options.realtime_clock    = EnvFlag("KYTY_REALTIME_CLOCK", options.realtime_clock);
	options.dpp_skip_inactive = EnvFlag("KYTY_DPP_SKIP_INACTIVE", options.dpp_skip_inactive);
	options.lane_reductions   = EnvFlag("KYTY_LANE_REDUCTIONS", options.lane_reductions);
	options.vs_launched_exec = EnvFlag("KYTY_VS_LAUNCHED_EXEC", options.vs_launched_exec);
	options.ir_linear_uses    = EnvFlag("KYTY_IR_LINEAR_USES", options.ir_linear_uses);
	options.fold_lane_masks   = EnvFlag("KYTY_FOLD_LANE_MASKS", options.fold_lane_masks);
	options.spirv_optimize    = EnvFlag("KYTY_SPIRV_OPT", options.spirv_optimize);
	options.spirv_optimize_extended =
	    EnvFlag("KYTY_SPIRV_OPT_EXTENDED", options.spirv_optimize_extended);
	if (const auto* cap = std::getenv("KYTY_DISPATCHER_CAP"); cap != nullptr && cap[0] != '\0') {
		options.dispatcher_cap = static_cast<uint32_t>(std::strtoul(cap, nullptr, 0));
	}
	// KYTY_NATIVE_INDIRECT_MESH=1|on|verify|exit, or unset/empty (the default, renderer/meshIndirect.h:
	// GPU-converted indirect mesh draws); 0 and "empty" keep the pushed-dword-only mesh draw
	// parameters.
	{
		const auto* mode = std::getenv("KYTY_NATIVE_INDIRECT_MESH");
		options.mesh_indirect_params =
		    mode == nullptr || mode[0] == '\0' || std::strcmp(mode, "1") == 0 ||
		    std::strcmp(mode, "on") == 0 || std::strcmp(mode, "verify") == 0 ||
		    std::strcmp(mode, "exit") == 0;
	}
	if (const auto* mode = std::getenv("KYTY_MAD_MODE"); mode != nullptr) {
		if (std::strcmp(mode, "exact") == 0) {
			options.mad_mode = MadMode::Exact;
		} else if (std::strcmp(mode, "fused") == 0) {
			options.mad_mode = MadMode::Fused;
		} else if (std::strcmp(mode, "position") == 0) {
			options.mad_mode = MadMode::Position;
		}
	}
	options.function_lds_compact = EnvFlag("KYTY_FUNCTION_LDS_COMPACT", options.function_lds_compact);
	options.astro_hardware_rt = EnvFlag("KYTY_HW_RT_ASTRO", false) && EnvFlag("KYTY_HW_RT_BACKEND", false);
	if (const auto* hash = std::getenv("KYTY_BVH_CAPTURE_SHADER"); hash != nullptr) {
		if (std::strcmp(hash, "all") == 0) options.bvh_capture_shader = UINT64_MAX;
		else {
			char* end = nullptr;
			const auto value = std::strtoull(hash, &end, 16);
			if (end != hash && *end == '\0') options.bvh_capture_shader = value;
		}
	}
	if (const auto* hash = std::getenv("KYTY_SCALAR_READ_PROBE_SHADER"); hash && *hash) {
		char* end = nullptr;
		const auto value = std::strtoull(hash, &end, 16);
		if (end != hash && *end == '\0') options.scalar_read_probe_shader = value;
	}
	std::vector<uint64_t> probe_pcs;
	ParseHashList(std::getenv("KYTY_SCALAR_READ_PROBE_PCS"), probe_pcs);
	for (const auto pc: probe_pcs)
		if (pc <= UINT32_MAX && (pc & 3) == 0) options.scalar_read_probe_pcs.push_back(pc);
	return options;
}

CodegenOptions& Storage() {
	static CodegenOptions options = FromEnvironment();
	return options;
}

} // namespace

const CodegenOptions& GetCodegenOptions() {
	return Storage();
}

bool LoopGuardApplies(uint64_t shader_hash) {
	const auto& options = Storage();
	return options.loop_guard_budget != 0 &&
	       std::ranges::find(options.loop_guard_shaders, shader_hash) !=
	           options.loop_guard_shaders.end();
}

bool BindlessStridedComputeApplies(uint64_t shader_hash) {
	const auto& options = Storage();
	return options.bindless_strided_compute ||
	       std::ranges::find(options.bindless_strided_compute_shaders, shader_hash) !=
	           options.bindless_strided_compute_shaders.end();
}

bool BdaWritesApplies(uint64_t shader_hash) {
	const auto& options = Storage();
	return BdaWriteCandidatesApplies(shader_hash) ||
	       std::ranges::find(options.bda_writes_shaders, shader_hash) !=
	       options.bda_writes_shaders.end();
}

bool SrtRuntimeDataReadsApplies(uint64_t shader_hash) {
	const auto& shaders = Storage().srt_runtime_data_read_shaders;
	return std::ranges::find(shaders, shader_hash) != shaders.end();
}

bool BdaWritesEnabled() {
	return !Storage().bda_writes_shaders.empty() || Storage().bda_write_mode != BdaWriteMode::Off;
}

bool BvhCaptureApplies(uint64_t shader_hash) {
	const auto selected = Storage().bvh_capture_shader;
	return selected != 0 && (selected == UINT64_MAX || selected == shader_hash);
}

bool BdaWritesDeferredEnabled() {
	return Storage().bda_write_mode == BdaWriteMode::Deferred;
}

bool BdaWriteCandidatesApplies(uint64_t shader_hash) {
	return Storage().bda_write_mode != BdaWriteMode::Off &&
	       (shader_hash == BdaWaterLightingHash || shader_hash == BdaShadowResolveHash);
}
bool BdaWriteCandidatesVerify() {
	return Storage().bda_write_mode == BdaWriteMode::CandidatesVerify;
}
uint32_t BdaWriteCandidateStorePc(uint64_t shader_hash) {
	return shader_hash == BdaWaterLightingHash ? 0x530u
	     : shader_hash == BdaShadowResolveHash ? 0x1b4u : UINT32_MAX;
}

void SetCodegenOptions(const CodegenOptions& options) {
	Storage() = options;
}

} // namespace Libs::Graphics::ShaderRecompiler
