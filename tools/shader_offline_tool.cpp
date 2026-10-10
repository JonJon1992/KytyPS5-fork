// Offline translation of one guest shader stored in a program cache (_PipelineCache/*.programs.bin):
// finds its source key by guest hash, runs ShaderRecompiler::TranslateProgram with the IR dump, and
// reports whether resource tracking skips it (KYTY_SRT_VARIANT_READS) and at which pc.
//
// Usage: shader_offline_tool <programs.bin> <hex guest hash> [--dump]
//        shader_offline_tool <programs.bin> --all   (every source: result and bindless images)
// Codegen switches come from the environment as in the emulator (run it with the run's KYTY_*
// variables, e.g. KYTY_SRT_VARIANT_READS=1 KYTY_BINDLESS_STRIDED_COMPUTE=1). --dump prints the
// decoded ISA and the IR resource tracking sees (KYTY_DUMP_STDOUT).

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace Libs::Graphics;

// One source key (ProgramDiskCache::BuildSourceKey, layout 5) parsed from its bytes.
struct SourceKey {
	uint32_t              stage           = 0;
	uint64_t              hash            = 0;
	uint32_t              user_data_count = 0;
	uint32_t              wave_size       = 64;
	uint32_t              user_data_base  = 0;
	bool                  plain_mip_stats = false;
	bool                  bindless_images = false;
	bool                  bindless_samplers = false;
	std::vector<uint32_t> static_state;
	std::vector<uint32_t> code;
	std::vector<uint32_t> back_code;
};

class Reader {
public:
	Reader(const std::vector<uint8_t>& bytes, size_t at): m_bytes(bytes), m_at(at) {}

	template <typename T>
	bool Get(T& value) {
		if (m_at + sizeof(T) > m_bytes.size()) return false;
		std::memcpy(&value, m_bytes.data() + m_at, sizeof(T));
		m_at += sizeof(T);
		return true;
	}
	bool Words(std::vector<uint32_t>& words) {
		uint32_t count = 0;
		if (!Get(count) || m_at + uint64_t {count} * 4u > m_bytes.size()) return false;
		words.resize(count);
		std::memcpy(words.data(), m_bytes.data() + m_at, uint64_t {count} * 4u);
		m_at += uint64_t {count} * 4u;
		return true;
	}
	[[nodiscard]] uint8_t Peek() const { return m_at < m_bytes.size() ? m_bytes[m_at] : 0; }
	void Skip(size_t bytes) { m_at += bytes; }

private:
	const std::vector<uint8_t>& m_bytes;
	size_t                      m_at;
};

// The source key at `at` (its layout word), if the bytes there parse as one.
std::optional<SourceKey> ParseSource(const std::vector<uint8_t>& file, size_t at) {
	constexpr uint32_t layout = 5;
	if (at + 16 > file.size() || std::memcmp(file.data() + at, &layout, 4) != 0) {
		return std::nullopt;
	}
	Reader reader(file, at + 4);
	SourceKey key;
	uint32_t code_size = 0;
	uint8_t flags[6] {};
	if (!reader.Get(key.stage) || key.stage == 0 || key.stage > 8 || !reader.Get(key.hash) ||
	    !reader.Get(key.user_data_count) || !reader.Get(code_size) || code_size == 0 ||
	    !reader.Words(key.static_state) || !reader.Get(key.wave_size) ||
	    (key.wave_size != 32 && key.wave_size != 64) || !reader.Get(key.user_data_base)) {
		return std::nullopt;
	}
	for (auto& flag: flags) {
		if (!reader.Get(flag) || flag > 3) return std::nullopt;
	}
	key.plain_mip_stats   = flags[0] != 0;
	key.bindless_images   = flags[1] != 0;
	key.bindless_samplers = flags[2] != 0;
	// KYTY_SRT_RUNTIME_DATA_READS marker (keyed only when set).
	if (reader.Peek() == 0x5d) reader.Skip(1);
	if (!reader.Words(key.code) || !reader.Words(key.back_code) || key.code.size() != code_size) {
		return std::nullopt;
	}
	return key;
}

std::optional<SourceKey> FindSource(const std::vector<uint8_t>& file, uint64_t hash) {
	for (size_t at = 0; at + 16 <= file.size(); ++at) {
		if (std::memcmp(file.data() + at + 8, &hash, 8) == 0) {
			if (auto key = ParseSource(file, at)) return key;
		}
	}
	return std::nullopt;
}

ShaderType StageOf(uint32_t stage) { return static_cast<ShaderType>(stage); }

// The compute input a program was translated with (BuildStageStaticKey's compute layout,
// src/graphics/shader/shaderStaticKey.cpp): thread ids and sizes decide which values are uniform.
ShaderComputeInputInfo DecodeComputeStaticKey(std::span<const uint32_t> key) {
	ShaderComputeInputInfo info {};
	if (key.size() < 14) return info;
	info.workgroup_register         = static_cast<int>(key[0]);
	info.wave_size                  = key[1] & 0xffu;
	info.float_mode                 = static_cast<uint8_t>(key[1] >> 8u);
	info.host_subgroup_size         = key[2];
	info.thread_ids_num             = static_cast<int>(key[3]);
	info.lds_size_dwords            = key[4];
	info.scratch_size_dwords        = key[5];
	info.dispatch_thread_dimensions = key[6] != 0;
	for (uint32_t i = 0; i < 3; ++i) {
		info.threads_num[i] = key[7 + i * 2];
		info.group_id[i]    = key[8 + i * 2] != 0;
	}
	info.tg_size_en = key[13] != 0;
	return info;
}

ShaderRecompiler::TranslateResult Translate(const SourceKey& key, bool dump) {
	auto compute = DecodeComputeStaticKey(key.static_state);
	ShaderPixelInputInfo   pixel {};
	ShaderVertexInputInfo  vertex {};
	ShaderRecompiler::CompileOptions options;
	options.stage             = StageOf(key.stage);
	options.wave_size         = key.wave_size;
	options.user_data_base    = key.user_data_base;
	options.shader_hash       = key.hash;
	options.dump_ir           = dump;
	options.early_dump        = dump;
	options.back_code         = key.back_code;
	options.bindless_images   = key.bindless_images;
	options.bindless_samplers = key.bindless_samplers;
	options.plain_mip_stats_variant = key.plain_mip_stats;
	switch (options.stage) {
		case ShaderType::Compute: options.input_info.compute = &compute; break;
		case ShaderType::Pixel: options.input_info.pixel = &pixel; break;
		default: options.input_info.vertex = &vertex; break;
	}
	return ShaderRecompiler::TranslateProgram(key.code, options);
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: %s <programs.bin> <hex guest hash> [--dump]\n", argv[0]);
		return 2;
	}
	const bool dump = argc > 3 && std::strcmp(argv[3], "--dump") == 0;
	if (dump) setenv("KYTY_DUMP_STDOUT", "1", 1);

	static Common::Subsystems subsystems;
	Common::InitializeThreads();
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions config;
	config.printf_direction = Config::LogDirection::Silent;
	Config::Load(config);
	subsystems.Initialize<Log::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	ShaderInit();

	std::ifstream input(argv[1], std::ios::binary);
	const std::vector<uint8_t> file((std::istreambuf_iterator<char>(input)), {});
	if (std::strcmp(argv[2], "--all") == 0) {
		// One line per distinct source: hash stage result images bindless bindless-compare.
		std::vector<uint64_t> seen;
		for (size_t at = 0; at + 16 <= file.size(); ++at) {
			const auto key = ParseSource(file, at);
			if (!key) continue;
			uint64_t identity = key->hash ^ (uint64_t {key->stage} << 56u);
			for (const auto word: key->static_state) identity = identity * 0x100000001b3ull ^ word;
			if (std::ranges::find(seen, identity) != seen.end()) continue;
			seen.push_back(identity);
			// A child per source: a translation that exits (missing static state the tool does
			// not decode) is reported instead of ending the scan.
			std::fflush(stdout);
			const pid_t child = fork();
			if (child == 0) {
				const auto result = Translate(*key, false);
				uint32_t bindless = 0, compare = 0;
				for (const auto& image: result.program.info.images) {
					bindless += image.bindless ? 1u : 0u;
					compare += image.bindless && image.depth_compare ? 1u : 0u;
				}
				std::printf("0x%016" PRIx64 " %u %s %zu %u %u\n", key->hash, key->stage,
				            result.skip_dispatch ? "SKIPPED" : "ok",
				            result.program.info.images.size(), bindless, compare);
				std::fflush(stdout);
				_exit(0);
			}
			int status = 0;
			waitpid(child, &status, 0);
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
				std::printf("0x%016" PRIx64 " %u EXITED\n", key->hash, key->stage);
			}
		}
		return 0;
	}
	const auto hash = std::strtoull(argv[2], nullptr, 16);
	const auto key = FindSource(file, hash);
	if (!key) {
		std::fprintf(stderr, "shader 0x%016" PRIx64 " not found\n", hash);
		return 1;
	}
	std::printf("shader 0x%016" PRIx64 " stage=%u wave=%u code=%zu words bindless=%d/%d\n", key->hash,
	            key->stage, key->wave_size, key->code.size(), key->bindless_images,
	            key->bindless_samplers);
	const auto result = Translate(*key, dump);
	std::printf("result: %s\n", result.skip_dispatch ? "SKIPPED" : "translated");
	const auto& program = result.program;
	for (size_t index = 0; index < program.info.images.size(); ++index) {
		const auto& image = program.info.images[index];
		const auto* source = image.source < program.descriptor_sources.size()
		                         ? &program.descriptor_sources[image.source] : nullptr;
		const bool indirect = source != nullptr && source->indirect_image.has_value();
		std::printf("image %zu: class=%u bindless=%d compare=%d indirect=%d table_words=%u "
		            "key_count_imm=%d\n",
		            index, static_cast<uint32_t>(image.resource_class), image.bindless,
		            image.depth_compare, indirect,
		            indirect ? program.descriptor_sources[source->indirect_image->table_source].dword_count : 0u,
		            indirect && source->indirect_image->key_count.Resolve().IsImmediate()
		                ? static_cast<int>(source->indirect_image->key_count.Resolve().U32()) : -1);
	}
	return result.skip_dispatch ? 3 : 0;
}
