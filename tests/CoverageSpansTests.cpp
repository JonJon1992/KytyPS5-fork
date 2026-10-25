#include "kernel/coverageSpans.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using Libs::LibKernel::Memory::CoverageChanges;
using Libs::LibKernel::Memory::CoverageSpan;
using Libs::LibKernel::Memory::FindCoverageSpan;

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "CoverageSpansTests: failed: %s\n", message);
		std::abort();
	}
}

constexpr uint64_t Page  = 0x1000;
constexpr uint64_t Base  = 0x100000;
constexpr uint32_t Pages = 256;

// The spans of committed pages, as VirtualRanges::BuildSpansUnlocked joins adjacent ranges.
std::vector<CoverageSpan> Build(const std::vector<bool>& committed) {
	std::vector<CoverageSpan> spans;
	for (uint32_t page = 0; page < committed.size(); page++) {
		if (!committed[page]) {
			continue;
		}
		const uint64_t start = Base + page * Page;
		if (!spans.empty() && spans.back().end == start) {
			spans.back().end = start + Page;
		} else {
			spans.push_back({start, start + Page});
		}
	}
	return spans;
}

void TestTouchingChanges() {
	const std::vector<CoverageSpan> spans {{0x2000, 0x4000}, {0x8000, 0x9000}};
	CoverageChanges                 changes;
	changes.Note(0x4000, 0x5000); // right next to the first span: may join it
	auto span = FindCoverageSpan(spans, 0x3000);
	Check(changes.Touches(span.start, span.end), "a change at a span's end must count");
	span = FindCoverageSpan(spans, 0x8800);
	Check(!changes.Touches(span.start, span.end), "a change away from a span must not count");
	span = FindCoverageSpan(spans, 0x4800);
	Check(span.start == 0x4800 && span.end == 0x4800, "an uncovered address has an empty span");
	Check(changes.Touches(span.start, span.end), "a change over an uncovered address must count");
	changes.Clear();
	for (uint32_t i = 0; i <= CoverageChanges::Capacity; i++) {
		changes.Note(0x100000 + i * Page, 0x100000 + i * Page + Page);
	}
	span = FindCoverageSpan(spans, 0x8800);
	Check(changes.Touches(span.start, span.end), "past the capacity every lookup must count");
}

// Random coverage changes and lookups: an answer from the old spans that no change touches must
// equal the answer of spans built anew.
void TestRandomAgainstRebuild() {
	std::mt19937              random(12345);
	std::vector<bool>         committed(Pages, false);
	std::vector<CoverageSpan> spans = Build(committed);
	CoverageChanges           changes;
	uint64_t                  kept = 0;
	for (int step = 0; step < 400000; step++) {
		if (random() % 4 == 0) {
			const uint32_t first = random() % Pages;
			const uint32_t last  = std::min<uint32_t>(Pages, first + 1 + random() % 24);
			const bool     value = random() % 2 == 0;
			for (uint32_t page = first; page < last; page++) {
				committed[page] = value;
			}
			changes.Note(Base + first * Page, Base + last * Page);
			continue;
		}
		const uint64_t vaddr = Base - Page + random() % ((Pages + 2) * Page);
		const auto     span  = FindCoverageSpan(spans, vaddr);
		if (changes.Touches(span.start, span.end)) {
			spans = Build(committed);
			changes.Clear();
			continue;
		}
		const auto exact = FindCoverageSpan(Build(committed), vaddr);
		Check(span == exact, "an untouched span must match the rebuilt one");
		kept++;
	}
	Check(kept > 10000, "most lookups should be answered without a rebuild");
}

} // namespace

int main() {
	TestTouchingChanges();
	TestRandomAgainstRebuild();
	std::puts("CoverageSpansTests: all cases passed");
	return 0;
}
