#include "graphics/host_gpu/renderer/cache/htileSliceState.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::HtileSliceState;

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "HtileSliceStateTests: failed: %s\n", message);
    std::abort();
  }
}

bool AllSlices(const HtileSliceState &state, bool cleared) {
  for (uint32_t slice = 0; slice < HtileSliceState::MaxSlices; slice++) {
    if (state.Test(slice) != cleared) {
      return false;
    }
  }
  return true;
}

void TestSeeds() {
  Check(AllSlices(HtileSliceState(true), true), "a cleared seed left a slice uncleared");
  Check(AllSlices(HtileSliceState(false), false), "an uncleared seed left a slice cleared");
  Check(AllSlices(HtileSliceState(), false), "the default state has a cleared slice");
}

// The draw path: a depth clear marks every slice, then each layer's draw consumes its own slice.
void TestConsumeEverySliceOfALargeArray() {
  HtileSliceState state(false);
  state.SetAll(true);
  for (uint32_t slice = 0; slice < HtileSliceState::MaxSlices; slice++) {
    Check(state.Test(slice), "a cleared slice read as consumed");
    Check(state.Set(slice, false), "consuming a cleared slice reported no change");
    Check(!state.Test(slice), "a consumed slice still read as cleared");
    Check(!state.Set(slice, false), "consuming a slice twice reported a change");
    if (slice + 1u < HtileSliceState::MaxSlices) {
      Check(state.Test(slice + 1u), "consuming a slice consumed the next one");
    }
  }
  Check(AllSlices(state, false), "a consumed array kept a cleared slice");
  state.SetAll(true);
  Check(AllSlices(state, true), "a second depth clear missed a slice");
}

// A change past slice 63 must not disturb the slices it does not name, on either side of the
// stored words.
void TestHighSliceKeepsTheOthers() {
  HtileSliceState state(true);
  Check(state.Set(1000u, false), "consuming slice 1000 reported no change");
  for (uint32_t slice = 0; slice < HtileSliceState::MaxSlices; slice++) {
    Check(state.Test(slice) == (slice != 1000u), "slice 1000 changed another slice");
  }
  HtileSliceState uncleared(false);
  Check(uncleared.Set(2047u, true), "clearing the last slice reported no change");
  Check(uncleared.Test(2047u) && !uncleared.Test(2046u) && !uncleared.Test(63u) &&
            !uncleared.Test(64u),
        "clearing the last slice changed another slice");
}

void TestWordsMatchTheBits() {
  HtileSliceState state(false);
  state.Set(0u, true);
  state.Set(63u, true);
  state.Set(64u, true);
  state.Set(130u, true);
  Check(state.Word(0) == ((uint64_t{1} << 63u) | 1u), "word 0 does not match slices 0 and 63");
  Check(state.Word(1) == 1u, "word 1 does not match slice 64");
  Check(state.Word(2) == (uint64_t{1} << 2u), "word 2 does not match slice 130");
  Check(state.Word(31) == 0u, "an unstored word is not the uncleared default");
  state.SetAll(true);
  Check(state.Word(31) == ~uint64_t{0}, "an unstored word is not the cleared default");
}

void TestOutOfRangeSlices() {
  HtileSliceState state(true);
  Check(!state.Test(HtileSliceState::MaxSlices), "a slice past the hardware range read as set");
  Check(!state.Set(HtileSliceState::MaxSlices, false),
        "a slice past the hardware range was stored");
  Check(AllSlices(state, true), "an out-of-range change disturbed a valid slice");
}

void TestEquality() {
  HtileSliceState a(true);
  HtileSliceState b(false);
  b.SetAll(true);
  Check(a == b, "equal states compare unequal");
  a.Set(500u, false);
  Check(!(a == b), "different states compare equal");
  b.Set(500u, false);
  Check(a == b, "states equal after the same change compare unequal");
}

} // namespace

int main() {
  TestSeeds();
  TestConsumeEverySliceOfALargeArray();
  TestHighSliceKeepsTheOthers();
  TestWordsMatchTheBits();
  TestOutOfRangeSlices();
  TestEquality();
  std::puts("HtileSliceStateTests: passed");
  return 0;
}
