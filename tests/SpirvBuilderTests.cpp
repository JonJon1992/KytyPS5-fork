#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
#include <vector>

namespace {
thread_local bool count_allocations = false;
thread_local size_t allocations = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "SpirvBuilderTests: %s\n", message);
		std::exit(1);
	}
}
} // namespace

// Observe the real allocator only inside the warmed declaration lookups below.
void* operator new(size_t size) {
	if (count_allocations) ++allocations;
	if (void* memory = std::malloc(std::max(size, size_t {1}))) return memory;
	std::abort();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

namespace Libs::Graphics::ShaderRecompiler::Spirv {
namespace {

void TestDeclarationIdentityAndEncoding() {
	Builder builder;
	const auto u32 = builder.Type(spv::OpTypeInt, 32u, 0u);
	const auto pair = builder.Type(spv::OpTypeVector, u32, 2u);
	const auto positive = builder.Constant(spv::OpConstant, u32, 0x7fffffffu);
	const auto negative = builder.Constant(spv::OpConstant, u32, -1);
	Check(negative == builder.Constant(spv::OpConstant, u32, 0xffffffffu),
	      "signed literal changed its encoded bits");
	const std::array values {positive, negative};
	const auto composite = builder.Constant(spv::OpConstantComposite, pair, std::span(values));
	Check(composite == builder.Constant(spv::OpConstantComposite, pair, positive, negative),
	      "span and scalar operands did not reuse the same declaration");
	const std::array members {u32, pair};
	const auto plain = builder.Type(spv::OpTypeStruct, std::span(members));
	Check(plain == builder.Type(spv::OpTypeStruct, u32, pair), "type keys depend on operand container");
	Check(plain == builder.DecoratedType(spv::OpTypeStruct, {}, std::span(members)),
	      "empty annotations changed type identity");
	const auto block = builder.DecoratedType(
	    spv::OpTypeStruct, {{spv::OpDecorate, {spv::DecorationBlock}}}, std::span(members));
	Check(block != plain, "decorated and undecorated types were merged");
	Check(block == builder.DecoratedType(
	                   spv::OpTypeStruct, {{spv::OpDecorate, {spv::DecorationBlock}}}, u32, pair),
	      "decorated type lookup did not reuse its ID");
	const std::vector<uint32_t> expected {
	    0x07230203, 0x00010300, 0, 8, 0,
	    0x00030047, 7, 2,             // OpDecorate %7 Block
	    0x00040015, 1, 32, 0,         // OpTypeInt %1 32 0
	    0x00040017, 2, 1, 2,          // OpTypeVector %2 %1 2
	    0x0004002b, 1, 3, 0x7fffffff, // OpConstant %1 %3 0x7fffffff
	    0x0004002b, 1, 4, 0xffffffff, // OpConstant %1 %4 0xffffffff
	    0x0005002c, 2, 5, 3, 4,       // OpConstantComposite %2 %5 %3 %4
	    0x0004001e, 6, 1, 2,          // OpTypeStruct %6 %1 %2
	    0x0004001e, 7, 1, 2,          // OpTypeStruct %7 %1 %2 (decorated)
	};
	Check(builder.Build() == expected, "declaration interning changed the SPIR-V serialization");
	Check(builder.Type(spv::OpTypeStruct, u32) != plain, "key length was ignored");
	Check(builder.Type(spv::OpTypeStruct, std::span<const uint32_t> {}) ==
	          builder.Type(spv::OpTypeStruct), "empty operand containers changed type identity");
	Check(builder.Constant(spv::OpConstantNull, u32) ==
	          builder.Constant(spv::OpConstantNull, u32, std::span<const uint32_t> {}),
	      "empty constant operands changed declaration identity");
	Check(builder.Type(spv::OpTypePointer, spv::StorageClassFunction, u32) ==
	          builder.Type(spv::OpTypePointer, uint32_t {spv::StorageClassFunction}, u32),
	      "enum and encoded scalar operands did not share an ID");
}

void TestScalarDeclarationHitsDoNotAllocate() {
	Builder builder;
	const auto type = builder.Type(spv::OpTypeInt, 32u, 0u);
	const auto value = builder.Constant(spv::OpConstant, type, 7u);
	allocations = 0;
	count_allocations = true;
	bool same = true;
	for (size_t i = 0; i < 4096; ++i) {
		same &= builder.Type(spv::OpTypeInt, 32u, 0u) == type;
		same &= builder.Constant(spv::OpConstant, type, 7u) == value;
	}
	count_allocations = false;
	Check(same, "a warmed declaration returned a different ID");
	if (allocations != 0) std::fprintf(stderr, "warmed lookups allocated %zu times\n", allocations);
	Check(allocations == 0, "scalar declaration cache hits allocate temporary keys");
}

void TestSpanDeclarationMissesDoNotCopyAnOwnedKey() {
	const auto measure = [](bool span) {
		Builder builder;
		const auto type = builder.Type(spv::OpTypeInt, 32u, 0u);
		allocations = 0;
		count_allocations = true;
		for (uint32_t value = 0; value < 128; ++value) {
			if (span) (void)builder.Constant(spv::OpConstant, type, std::span(&value, 1));
			else (void)builder.Constant(spv::OpConstant, type, value);
		}
		count_allocations = false;
		return allocations;
	};
	const auto scalar = measure(false);
	const auto span = measure(true);
	if (span > scalar) std::fprintf(stderr, "miss allocations: scalar=%zu span=%zu\n", scalar, span);
	Check(span <= scalar, "span declaration misses copied an already owned key");
}

void Benchmark() {
	constexpr uint32_t iterations = 1000000;
	constexpr uint32_t distinct = 1024;
	Builder builder;
	const auto type = builder.Type(spv::OpTypeInt, 32u, 0u);
	std::array<uint32_t, distinct> input {};
	for (uint32_t i = 0; i < distinct; ++i) {
		input[i] = i * 2654435761u;
		(void)builder.Constant(spv::OpConstant, type, input[i]);
	}
	std::puts("round,lookups,allocations,ns_per_lookup,checksum");
	for (uint32_t round = 0; round < 9; ++round) {
		uint64_t checksum = 0;
		allocations = 0;
		const auto begin = std::chrono::steady_clock::now();
		count_allocations = true;
		for (uint32_t i = 0; i < iterations; ++i)
			checksum += builder.Constant(spv::OpConstant, type, input[i & (distinct - 1u)]);
		count_allocations = false;
		const double ns = std::chrono::duration<double, std::nano>(
		                      std::chrono::steady_clock::now() - begin).count() / iterations;
		std::printf("%u,%u,%zu,%.3f,%llu\n", round, iterations, allocations, ns,
		            static_cast<unsigned long long>(checksum));
	}
}
} // namespace
} // namespace Libs::Graphics::ShaderRecompiler::Spirv

namespace Common {
int DbgExitIfHandler(const char* expression, const char* file, int line) {
	std::fprintf(stderr, "SpirvBuilderTests: %s at %s:%d\n", expression, file, line);
	std::abort();
}
void DbgExit(int) { std::abort(); }
} // namespace Common

int main(int argc, char** argv) {
	using namespace Libs::Graphics::ShaderRecompiler::Spirv;
	if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) {
		Benchmark();
		return 0;
	}
	TestDeclarationIdentityAndEncoding();
	TestSpanDeclarationMissesDoNotCopyAnOwnedKey();
	TestScalarDeclarationHitsDoNotAllocate();
	std::puts("SpirvBuilderTests: all cases passed");
}
