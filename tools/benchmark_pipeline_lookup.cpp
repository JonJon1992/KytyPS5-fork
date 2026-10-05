// CPU-only microbenchmark of the old/new lookup mechanisms with the renderer's key layout.
// No Vulkan calls, shader compilation, key construction per draw, or FPS measurement.
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineLookupMemo.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <xxhash.h>

namespace {
using namespace Libs::Graphics;

struct Key {
	PipelineRenderingState rendering;
	std::array<uint64_t, 3> vertex_shader_ids {};
	uint64_t ps_shader_id = 0;
	PipelineVertexInputState vertex_input;
	PipelineStaticParameters static_params;
	bool operator==(const Key& other) const {
		return rendering == other.rendering && vertex_shader_ids == other.vertex_shader_ids &&
		       ps_shader_id == other.ps_shader_id && vertex_input == other.vertex_input &&
		       std::memcmp(&static_params, &other.static_params, sizeof(static_params)) == 0;
	}
};

struct Equal {
	bool active;
	bool operator()(const Key& a, const Key& b) const {
		if (active) return a == b;
		return a.rendering == b.rendering && a.vertex_shader_ids == b.vertex_shader_ids &&
		       a.ps_shader_id == b.ps_shader_id &&
		       a.vertex_input.binding_count == b.vertex_input.binding_count &&
		       a.vertex_input.attribute_count == b.vertex_input.attribute_count &&
		       a.vertex_input.bindings == b.vertex_input.bindings &&
		       a.vertex_input.attributes == b.vertex_input.attributes &&
		       std::memcmp(&a.static_params, &b.static_params, sizeof(a.static_params)) == 0;
	}
};

struct Hash {
	bool fast;
	static void Mix(size_t& hash, size_t value) {
		hash ^= value + size_t(0x9e3779b97f4a7c15ull) + (hash << 6u) + (hash >> 2u);
	}
	size_t operator()(const Key& key) const {
		size_t hash = 0;
		Mix(hash, key.rendering.color_count);
		for (uint32_t i = 0; i < key.rendering.color_count; ++i)
			Mix(hash, static_cast<uint32_t>(key.rendering.color_formats[i]));
		Mix(hash, static_cast<uint32_t>(key.rendering.depth_format));
		Mix(hash, static_cast<uint32_t>(key.rendering.stencil_format));
		for (auto id : key.vertex_shader_ids) Mix(hash, id);
		Mix(hash, key.ps_shader_id);
		Mix(hash, key.vertex_input.binding_count);
		for (uint32_t i = 0; i < key.vertex_input.binding_count; ++i) {
			Mix(hash, key.vertex_input.bindings[i].stride);
			Mix(hash, key.vertex_input.bindings[i].instance);
		}
		Mix(hash, key.vertex_input.attribute_count);
		for (uint32_t i = 0; i < key.vertex_input.attribute_count; ++i) {
			Mix(hash, key.vertex_input.attributes[i].offset);
			Mix(hash, key.vertex_input.attributes[i].binding);
		}
		if (fast) {
			Mix(hash, XXH3_64bits(&key.static_params, sizeof(key.static_params)));
		} else {
			const auto* bytes = reinterpret_cast<const uint8_t*>(&key.static_params);
			for (size_t i = 0; i < sizeof(key.static_params); ++i) Mix(hash, bytes[i]);
		}
		return hash;
	}
};

class Lookup {
public:
	Lookup(bool multi, bool fast, const std::vector<Key>& keys)
	    : m_multi(multi), m_hash{fast}, m_map(0, m_hash, Equal{multi}) {
		for (size_t i = 0; i < keys.size(); ++i) m_map.emplace(keys[i], int(i + 1));
	}
	// Keep each call observable; otherwise a compiler can hoist an artificial repeated lookup.
#if defined(_MSC_VER)
	__declspec(noinline)
#else
	__attribute__((noinline))
#endif
	int Find(const Key& key) {
		size_t hash = 0;
		if (m_multi) {
			if (auto* value = m_memo.FindLast(this, 1, key)) return *value;
			hash = m_hash(key);
			if (auto* value = m_memo.Find(this, 1, key, hash)) return *value;
		} else if (m_last != nullptr && Equal{false}(m_last_key, key)) {
			return *m_last;
		}
		std::scoped_lock lock(m_mutex);
		const auto found = m_map.find(key);
		if (found == m_map.end()) return 0;
		if (m_multi) m_memo.Remember(this, 1, found->first, found->second, hash);
		else {
			m_last_key = key;
			m_last = &found->second;
		}
		return found->second;
	}
private:
	bool m_multi;
	Hash m_hash;
	std::unordered_map<Key, int, Hash, Equal> m_map;
	std::mutex m_mutex;
	Key m_last_key {};
	int* m_last = nullptr;
	PipelineLookupMemo<Key, int> m_memo;
};

void Measure(const char* mode, Lookup& lookup, const std::vector<Key>& keys, size_t cycle) {
	constexpr size_t iterations = 1'000'000;
	for (size_t i = 0; i < 10000; ++i) lookup.Find(keys[i % cycle]);
	uint64_t checksum = 0;
	const auto begin = std::chrono::steady_clock::now();
	for (size_t i = 0; i < iterations; ++i) checksum += lookup.Find(keys[i % cycle]);
	const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::steady_clock::now() - begin).count();
	std::printf("%s cycle=%zu ns/lookup=%.3f checksum=%llu\n", mode, cycle,
	            double(ns) / iterations, static_cast<unsigned long long>(checksum));
}
} // namespace

int main() {
	std::vector<Key> keys(64);
	for (size_t i = 0; i < keys.size(); ++i) {
		keys[i].vertex_shader_ids[0] = i + 1;
		keys[i].ps_shader_id = i + 2;
		keys[i].rendering.color_count = 1;
		keys[i].rendering.color_formats[0] = vk::Format::eR8G8B8A8Unorm;
		keys[i].static_params.topology = vk::PrimitiveTopology::eTriangleList;
	}
	std::printf("PipelineLookupMechanismBench: key_bytes=%zu iterations=1000000 (CPU only)\n", sizeof(Key));
	for (uint8_t inputs : {uint8_t(0), uint8_t(2), uint8_t(32)}) {
		for (auto& key : keys) {
			key.vertex_input.binding_count = inputs;
			key.vertex_input.attribute_count = inputs;
			for (uint8_t i = 0; i < inputs; ++i) {
				key.vertex_input.bindings[i].stride = 16;
				key.vertex_input.attributes[i] = {.offset = 4, .binding = i};
			}
		}
		Lookup old_byte(false, false, keys), old_batch(false, true, keys), updated(true, true, keys);
		std::printf("inputs=%u\n", static_cast<unsigned>(inputs));
		for (unsigned repeat = 0; repeat < 3; ++repeat) {
			for (size_t cycle : {size_t(1), size_t(2), size_t(64)}) {
				// Alternate measurement order to reduce systematic ordering bias.
				if (repeat % 2) Measure("new", updated, keys, cycle);
				Measure("old-byte", old_byte, keys, cycle);
				Measure("old-batch", old_batch, keys, cycle);
				if (repeat % 2 == 0) Measure("new", updated, keys, cycle);
			}
		}
	}
}
