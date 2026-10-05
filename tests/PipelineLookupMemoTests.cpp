#include "graphics/host_gpu/renderer/pipeline/pipelineLookupMemo.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <unordered_map>

namespace {

struct Key {
	std::array<uint32_t, 180> words {};
	Key() = default;
	Key(const Key&) = delete;
	Key& operator=(const Key&) = delete;
	bool operator==(const Key&) const = default;
};

using Memo = Libs::Graphics::PipelineLookupMemo<Key, int>;
int failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::printf("FAILED: %s\n", message);
		++failures;
	}
}

void CheckReuseAndCollisions() {
	Memo memo;
	int owner = 0, first = 1, second = 2, third = 3;
	Key a, b, c;
	b.words.back() = 1;
	c.words.back() = 2;
	constexpr size_t hash = 7; // Force all keys into the same hash and bucket.
	Check(memo.FindLast(&owner, 1, a) == nullptr, "empty memo misses");
	memo.Remember(&owner, 1, a, first, hash);
	memo.Remember(&owner, 1, b, second, hash);
	for (int repeat = 0; repeat < 3; ++repeat) {
		Check(memo.Find(&owner, 1, a, hash) == &first, "alternating key A hits");
		Check(memo.FindLast(&owner, 1, a) == &first, "table hit warms the last-key shortcut");
		Check(memo.Find(&owner, 1, b, hash) == &second, "alternating key B hits");
	}
	Check(memo.Find(&owner, 1, c, hash) == nullptr, "equal hash never substitutes a different key");
	Check(memo.Find(&owner, 1, b, hash + 512) == nullptr, "equal bucket never substitutes a different hash");
	memo.Remember(&owner, 1, c, third, hash);
	Check(memo.Find(&owner, 1, a, hash) == nullptr, "full bucket evicts its oldest insertion");
	Check(memo.Find(&owner, 1, b, hash) == &second, "full bucket keeps its other entry");
	Check(memo.Find(&owner, 1, c, hash) == &third, "full bucket stores the new entry");
}

void CheckLifetimeGuards() {
	Memo memo;
	int owner = 0, another_owner = 0, value = 1, replacement = 2;
	const Key query;
	auto key = std::make_unique<Key>();
	memo.Remember(&owner, 1, *key, value, 0);
	key.reset();
	// The old key is dangling: ASan catches any comparison before the lifetime guards.
	Check(memo.FindLast(&owner, 2, query) == nullptr, "last entry rejects an old generation before reading its key");
	Check(memo.Find(&owner, 2, query, 0) == nullptr, "table rejects an old generation before reading its key");
	Check(memo.FindLast(&another_owner, 1, query) == nullptr, "last entry rejects another cache before reading its key");
	Check(memo.Find(&another_owner, 1, query, 0) == nullptr, "table rejects another cache before reading its key");
	memo.Remember(&owner, 2, query, replacement, 0);
	Check(memo.FindLast(&owner, 2, query) == &replacement, "new generation exposes the replacement");
	Check(memo.Find(&owner, 3, query, 0) == nullptr, "a later generation cannot revive either old pipeline");
}

void CheckMapRehash() {
	Memo memo;
	std::unordered_map<unsigned, Key> keys;
	int owner = 0, value = 1;
	const auto& key = keys[0];
	memo.Remember(&owner, 1, key, value, 0);
	for (unsigned i = 1; i < 4096; ++i) keys[i].words[0] = i;
	const Key query;
	Check(memo.FindLast(&owner, 1, query) == &value, "unordered_map rehash preserves the remembered key");
	Check(memo.Find(&owner, 1, query, 0) == &value, "table entry survives unordered_map rehash");
}

} // namespace

int main() {
	CheckReuseAndCollisions();
	CheckLifetimeGuards();
	CheckMapRehash();
	std::printf("PipelineLookupMemoTests: %s (%d failures)\n", failures ? "failed" : "passed", failures);
	return failures ? 1 : 0;
}
