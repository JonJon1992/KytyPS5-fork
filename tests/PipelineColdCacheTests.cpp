#include "graphics/host_gpu/renderer/pipeline/pipelineBlendState.h"
#include "graphics/host_gpu/renderer/pipeline/pipelinePrefetchAdmission.h"

#include <cstdio>
#include <cstring>
#include <future>
#include <string>
#include <unordered_map>
#include <unordered_set>

int main() {
	using namespace Libs::Graphics;
	int failures = 0;
	const auto check = [&](bool condition, const char* message) {
		if (!condition) { std::printf("FAILED: %s\n", message); ++failures; }
	};
	struct Pending { std::future<int> future; uint64_t ticket; bool required = false; };
	std::unordered_map<int, Pending> pending;
	std::promise<int> running;
	pending.emplace(0, Pending {running.get_future(), 1});
	std::promise<int> consumed;
	auto consumer = consumed.get_future();
	pending.emplace(1, Pending {std::move(consumer), 2});
	consumer = std::move(pending.at(1).future); // Key reserved until CP publishes.
	consumed.set_value(11);
	check(FindCompletedPrefetchToRetire(pending) == pending.end(),
	      "running and consumer-reserved entries cannot be retired");
	for (int i = 2; i < 128; ++i) {
		std::promise<int> ready;
		auto future = ready.get_future();
		ready.set_value(i);
		pending.emplace(i, Pending {std::move(future), static_cast<uint64_t>(i + 1)});
	}
	// Pinning precedes moving the future: another preparer can run between these operations.
	pending.at(2).required = true;
	auto protected_oldest = FindCompletedPrefetchToRetire(pending);
	check(protected_oldest != pending.end() && protected_oldest->first == 3,
	      "completed result pinned for an imminent draw cannot be retired");
	pending.at(2).required = false;
	auto oldest = FindCompletedPrefetchToRetire(pending);
	check(oldest != pending.end() && oldest->first == 2,
	      "full storage reclaims oldest completed unused prediction");
	if (oldest != pending.end()) {
		check(oldest->second.future.get() == 2, "retired result remains owned for resource cleanup");
		pending.erase(oldest);
		std::promise<int> replacement;
		auto future = replacement.get_future();
		replacement.set_value(128);
		pending.emplace(128, Pending {std::move(future), 129});
		check(pending.size() == 128 && pending.contains(128), "new prediction admitted within bound");
	}
	check(consumer.get() == 11 && pending.contains(1), "reserved consuming draw keeps its exact result");
	running.set_value(10);
	oldest = FindCompletedPrefetchToRetire(pending);
	check(oldest != pending.end() && oldest->first == 0, "oldest running job becomes reclaimable only after completion");

	const auto min = static_cast<uint8_t>(Prospero::BlendOp::kMin);
	const auto max = static_cast<uint8_t>(Prospero::BlendOp::kMax);
	const auto add = static_cast<uint8_t>(Prospero::BlendOp::kAdd);
	const auto same = [](const auto& a, const auto& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; };
	PipelineStaticParameters a {}, b {};
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; ++slot) {
		a.blend_enable[slot] = true;
		a.color_comb_fcn[slot] = slot % 2 ? min : max;
		a.color_srcblend[slot] = 4;
		a.color_destblend[slot] = 5;
		a.color_mask[slot] = 15;
	}
	b = a;
	for (auto& factor: b.color_srcblend) factor = 1;
	for (auto& factor: b.color_destblend) factor = 0;
	NormalizeMinMaxBlendFactors(a);
	NormalizeMinMaxBlendFactors(b);
	check(same(a, b), "MIN/MAX unused factor changes share a pipeline key across every attachment");
	b = a;
	b.color_comb_fcn[0] = min;
	check(!same(a, b), "MIN and MAX remain different pipelines");
	a = {};
	a.blend_enable[0] = true;
	a.separate_alpha_blend[0] = true;
	a.color_comb_fcn[0] = min;
	a.alpha_comb_fcn[0] = add;
	a.color_srcblend[0] = 4;
	a.color_destblend[0] = 5;
	a.alpha_srcblend[0] = 6;
	a.alpha_destblend[0] = 7;
	NormalizeMinMaxBlendFactors(a);
	check(a.color_srcblend[0] == 0 && a.color_destblend[0] == 0 &&
	      a.alpha_srcblend[0] == 6 && a.alpha_destblend[0] == 7,
	      "RGB MIN removes factors while independent alpha ADD preserves them");
	a.color_comb_fcn[0] = add;
	a.alpha_comb_fcn[0] = max;
	a.color_srcblend[0] = 4;
	a.color_destblend[0] = 5;
	NormalizeMinMaxBlendFactors(a);
	check(a.color_srcblend[0] == 4 && a.color_destblend[0] == 5 &&
	      a.alpha_srcblend[0] == 0 && a.alpha_destblend[0] == 0,
	      "alpha MAX removes factors while RGB ADD preserves them");
	b = a;
	NormalizeMinMaxBlendFactors(b);
	check(same(a, b), "normalization is idempotent");
	a.blend_enable[0] = false;
	a.alpha_srcblend[0] = 9;
	b = a;
	NormalizeMinMaxBlendFactors(b);
	check(same(a, b), "disabled attachment is left to existing normalization");
	std::unordered_set<std::string> original_keys, normalized_keys;
	for (uint8_t rgb_src = 0; rgb_src < 4; ++rgb_src)
	for (uint8_t rgb_dst = 0; rgb_dst < 4; ++rgb_dst)
	for (uint8_t alpha_src = 0; alpha_src < 4; ++alpha_src)
	for (uint8_t alpha_dst = 0; alpha_dst < 4; ++alpha_dst) {
		PipelineStaticParameters variant {};
		variant.blend_enable[0] = variant.separate_alpha_blend[0] = true;
		variant.color_comb_fcn[0] = min;
		variant.alpha_comb_fcn[0] = max;
		variant.color_mask[0] = 15;
		variant.color_srcblend[0] = rgb_src;
		variant.color_destblend[0] = rgb_dst;
		variant.alpha_srcblend[0] = alpha_src;
		variant.alpha_destblend[0] = alpha_dst;
		original_keys.emplace(reinterpret_cast<const char*>(&variant), sizeof(variant));
		NormalizeMinMaxBlendFactors(variant);
		normalized_keys.emplace(reinterpret_cast<const char*>(&variant), sizeof(variant));
	}
	check(original_keys.size() == 256 && normalized_keys.size() == 1,
	      "256 equivalent MIN/MAX factor permutations collapse to one static key");
	std::printf("Synthetic MIN/MAX key permutations: %zu -> %zu\n", original_keys.size(), normalized_keys.size());
	if (failures == 0) std::puts("Cold pipeline cache: bounded reclamation and MIN/MAX equivalence passed");
	return failures != 0;
}
