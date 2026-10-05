#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Libs::Graphics {

// Per-thread lookup acceleration for immutable map keys and published pipelines. The owner
// supplies a generation unique to its lifetime, advanced whenever a pipeline is replaced.
// Check it before dereferencing a key: another cache may have destroyed the old map already.
// Keys/values must stay at stable addresses until that generation is invalidated.
// Two entries per bucket keep alternating keys hot even when their hashes collide. The
// last-key shortcut avoids hashing consecutive draws; no full pipeline key is copied.
template <typename Key, typename Value>
class PipelineLookupMemo {
public:
	Value* FindLast(const void* owner, uint64_t generation, const Key& key) const {
		return Matches(m_last, owner, generation, key) ? m_last.value : nullptr;
	}
	Value* Find(const void* owner, uint64_t generation, const Key& key, size_t hash) {
		const auto& bucket = m_buckets[hash % m_buckets.size()];
		for (size_t i = bucket.size(); i != 0; --i) {
			const auto& entry = bucket[i - 1];
			if (entry.hash == hash && Matches(entry, owner, generation, key)) {
				m_last = entry;
				return entry.value;
			}
		}
		return nullptr;
	}
	void Remember(const void* owner, uint64_t generation, const Key& key, Value& value, size_t hash) {
		auto& bucket = m_buckets[hash % m_buckets.size()];
		bucket[0] = bucket[1];
		bucket[1] = {owner, generation, &key, &value, hash};
		m_last = bucket[1];
	}

private:
	struct Entry {
		const void* owner = nullptr;
		uint64_t generation = 0;
		const Key* key = nullptr;
		Value* value = nullptr;
		size_t hash = 0;
	};
	static bool Matches(const Entry& entry, const void* owner, uint64_t generation, const Key& key) {
		return entry.owner == owner && entry.generation == generation && entry.key != nullptr &&
		       *entry.key == key;
	}
	Entry m_last;
	std::array<std::array<Entry, 2>, 512> m_buckets {};
};

} // namespace Libs::Graphics
