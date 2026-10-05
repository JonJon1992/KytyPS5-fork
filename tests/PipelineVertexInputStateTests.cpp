#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include <cstdio>

int main() {
	using Libs::Graphics::PipelineVertexInputState;
	int failures = 0;
	const auto check = [&](bool condition, const char* message) {
		if (!condition) {
			std::printf("FAILED: %s\n", message);
			++failures;
		}
	};
	PipelineVertexInputState a, b;
	b.bindings.back() = {.stride = 256, .instance = true};
	b.attributes.back() = {.offset = 64, .binding = 3};
	check(a == b, "empty mesh input ignores all inactive array entries");
	a.binding_count = b.binding_count = 1;
	a.attribute_count = b.attribute_count = 1;
	a.bindings[0] = b.bindings[0] = {.stride = 16, .instance = false};
	a.attributes[0] = b.attributes[0] = {.offset = 4, .binding = 0};
	check(a == b, "ordinary input ignores inactive trailing entries");
	b.bindings[0].stride = 32;
	check(!(a == b), "active stride distinguishes pipelines");
	b.bindings[0] = a.bindings[0];
	b.bindings[0].instance = true;
	check(!(a == b), "active input rate distinguishes pipelines");
	b.bindings[0] = a.bindings[0];
	b.attributes[0].offset = 8;
	check(!(a == b), "active attribute offset distinguishes pipelines");
	b.attributes[0] = a.attributes[0];
	b.attributes[0].binding = 1;
	check(!(a == b), "active attribute binding distinguishes pipelines");
	b.attributes[0] = a.attributes[0];
	b.binding_count = 2;
	check(!(a == b), "binding count distinguishes pipelines");
	b.binding_count = 1;
	b.attribute_count = 2;
	check(!(a == b), "attribute count distinguishes pipelines");
	a.binding_count = b.binding_count = a.bindings.size();
	a.attribute_count = b.attribute_count = a.attributes.size();
	check(!(a == b), "last entries participate when the arrays are fully active");
	b.bindings.back() = a.bindings.back();
	b.attributes.back() = a.attributes.back();
	check(a == b, "equal fully active inputs compare equally");
	a.binding_count = b.binding_count = 255;
	a.attribute_count = b.attribute_count = 255;
	check(a == b, "oversized counts do not read beyond the arrays");
	b.attributes.back().offset = 1;
	check(!(a == b), "bounded oversized comparisons still distinguish the stored entries");
	std::printf("PipelineVertexInputStateTests: %s (%d failures)\n", failures ? "failed" : "passed", failures);
	return failures ? 1 : 0;
}
