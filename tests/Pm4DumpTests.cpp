#include "common/file.h"
#include "graphics/guest_gpu/pm4.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
thread_local bool count_allocations = false;
thread_local size_t allocations = 0;
int failures = 0;
void Check(bool condition, const char* message) {
	if (!condition) { std::fprintf(stderr, "FAILED: %s\n", message); ++failures; }
}
}

void* operator new(size_t size) {
	if (count_allocations) ++allocations;
	if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
	std::abort();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

namespace {
namespace Pm4 = Libs::Graphics::Pm4;

size_t DumpAndCheck(uint32_t payload) {
	std::vector<uint32_t> words(payload + 3);
	words[0] = 0xdeadbeef; // The requested range starts after this word.
	words[1] = KYTY_PM4(payload + 1u, Pm4::IT_NOP, Pm4::R_ZERO);
	for (uint32_t i = 0; i < payload; ++i) words[i + 2] = 0xfedc0000u + i;
	words.back() = 0x80000000u; // Header-only type-2 padding.
	std::vector<char> buffer(size_t {payload} * 32 + 1024);
	Common::File file;
	Check(file.OpenInMem(buffer.data(), static_cast<uint32_t>(buffer.size())), "open dump buffer");
	allocations = 0;
	count_allocations = true;
	Pm4::DumpPm4PacketStream(&file, words.data(), 1, payload + 2);
	count_allocations = false;
	const size_t count = allocations;
	const std::string actual(buffer.data(), static_cast<size_t>(file.Tell()));
	char line[256];
	std::snprintf(line, sizeof(line), "----- Buffer --- dwords: 0x%05x, offset : 1, addr: %016" PRIx64 " ----- \r\n",
	              payload + 2, reinterpret_cast<uint64_t>(words.data()));
	std::string expected(line);
	std::snprintf(line, sizeof(line), "00001 | 0x%08x | IT_NOP R_ZERO(OP:0x10) SH:GX CNT:%u\r\n", words[1], payload);
	expected += line;
	for (uint32_t i = 0; i < payload; ++i) {
		std::snprintf(line, sizeof(line), "      | 0x%08x | \r\n", 0xfedc0000u + i);
		expected += line;
	}
	std::snprintf(line, sizeof(line), "%05x | 0x80000000 | <unsupported TYPE2 packet>\r\n", payload + 2);
	expected += line;
	Check(actual == expected, "dump preserves complete text, offset, padding and CRLF");
	return count;
}

void Bench() {
	constexpr uint32_t Payload = 16384, Repetitions = 20;
	std::vector<uint32_t> words(Payload + 1, 0x89abcdef);
	words[0] = KYTY_PM4(Payload + 1, Pm4::IT_DRAW_INDEX_AUTO, 0);
	std::vector<char> buffer(size_t {Payload} * 32 + 1024);
	Common::File file;
	Check(file.OpenInMem(buffer.data(), static_cast<uint32_t>(buffer.size())), "open benchmark buffer");
	const auto start = std::chrono::steady_clock::now();
	for (uint32_t i = 0; i < Repetitions; ++i) {
		file.Seek(0);
		Pm4::DumpPm4PacketStream(&file, words.data(), 0, static_cast<uint32_t>(words.size()));
	}
	const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
	std::printf("pm4-dump-bench: %.3f ns/dword\n", elapsed / (Payload * Repetitions));
}
}

int main(int argc, char** argv) {
	if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
		Bench();
		return failures != 0;
	}
	const auto short_dump = DumpAndCheck(8);
	const auto long_dump = DumpAndCheck(1024);
	DumpAndCheck(16384);
	std::printf("PM4 dump allocations: short=%zu long=%zu\n", short_dump, long_dump);
	Check(long_dump <= short_dump + 4, "PM4 dump allocation cost must not grow with each payload dword");
	if (!failures) std::puts("PM4 dump: text, boundaries and bounded allocations passed");
	return failures != 0;
}
