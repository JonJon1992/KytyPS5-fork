#include "graphics/host_gpu/sourceCopyTracker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <limits>
#include <semaphore>
#include <thread>

using Libs::Graphics::SourceCopyTracker;
using namespace std::chrono_literals;

namespace {
int failures = 0;
void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "SourceCopyTracker: %s\n", message);
        ++failures;
    }
}

void TestOverlapAndCompletion() {
    std::atomic<uint64_t> completed {0};
    SourceCopyTracker sources(completed);
    sources.Track(0x1000, 0x100, 1);
    sources.Track(0x1080, 0x100, 2);
    sources.Track(0x2000, 0x100, 3);
    Expect(sources.PendingValue(0x1000, 0x80) == 1, "left remainder retains producer A");
    Expect(sources.PendingValue(0x1080, 0x80) == 2, "overlap requires producer B");
    Expect(sources.PendingValue(0x1100, 0x80) == 2, "right extension belongs to B");
    Expect(sources.PendingValue(0x1180, 1) == 0, "end is exclusive");
    Expect(sources.PendingValue(0x1800, 0x100) == 0, "unrelated interval has no dependency");
    completed.store(1, std::memory_order_release);
    completed.notify_all();
    Expect(sources.PendingValue(0x1000, 0x80) == 0, "completion A releases only its remainder");
    Expect(sources.PendingValue(0x1080, 0x80) == 2, "completion A does not release B");
    Expect(sources.PendingValue(0x2000, 0x100) == 3, "other producer stays pending");
}

void TestNoGlobalWait() {
    std::atomic<uint64_t> completed {1};
    SourceCopyTracker sources(completed);
    sources.Track(0x1000, 16, 2);
    sources.Track(0x2000, 16, 3);
    auto guard = std::async(std::launch::async, [&] { return sources.WaitRange(0x1000, 16); });
    Expect(guard.wait_for(20ms) == std::future_status::timeout,
           "overlapping write waits while its copy is pending");
    completed.store(2, std::memory_order_release);
    completed.notify_all();
    const bool ready = guard.wait_for(250ms) == std::future_status::ready;
    // Always unblock a faulty implementation before joining.
    completed.store(3, std::memory_order_release);
    completed.notify_all();
    const auto waited = guard.get();
    Expect(ready && waited == 2, "write resumes after its producer without waiting for unrelated C");
    Expect(sources.WaitRange(0x3000, 16) == 0, "empty dependency returns immediately");
}

void TestOrderedSourceOverwrite() {
    std::array<uint8_t, 256> guest, staging {};
    guest.fill(0x41);
    std::atomic<uint64_t> completed {0};
    SourceCopyTracker sources(completed);
    const auto address = reinterpret_cast<uint64_t>(guest.data());
    sources.Track(address, guest.size(), 1);
    std::binary_semaphore may_copy {0};
    std::thread worker([&] {
        may_copy.acquire();
        std::memcpy(staging.data(), guest.data(), guest.size());
        completed.store(1, std::memory_order_release);
        completed.notify_all();
    });
    auto writer = std::async(std::launch::async, [&] {
        (void)sources.WaitRange(address, guest.size());
        guest.fill(0x72);
    });
    Expect(writer.wait_for(20ms) == std::future_status::timeout,
           "ordered emulator write cannot overwrite the pending source");
    may_copy.release();
    worker.join();
    writer.get();
    Expect(std::all_of(staging.begin(), staging.end(), [](auto byte) { return byte == 0x41; }),
           "earlier upload preserves its source version");
    Expect(std::all_of(guest.begin(), guest.end(), [](auto byte) { return byte == 0x72; }),
           "later emulator write becomes visible after the copy");
}

void TestCanonicalProjection() {
    std::atomic<uint64_t> completed {0};
    SourceCopyTracker sources(completed);
    struct View { uint64_t guest_address, size, backing; };
    const std::array<View, 2> views {{{0x10000, 0x80, 0x8000}, {0x20000, 0x80, 0x8040}}};
    sources.Track(std::span<const View>(views), 1, [](const View& view) { return view.backing; });
    Expect(sources.PendingValue(0x8000, 0xc0) == 1, "shared views retain canonical sources");
    Expect(sources.PendingValue(0x10000, 0x80) == 0, "guest VA is not the source identity");
    Expect(!sources.IsIdle(), "canonical sources remain pending");
    completed.store(1, std::memory_order_release);
    Expect(sources.IsIdle(), "completion releases canonical sources without map queries");
}

void TestBoundaryAndReuse() {
    std::atomic<uint64_t> completed {0};
    SourceCopyTracker sources(completed);
    const auto maximum = std::numeric_limits<uint64_t>::max();
    sources.Track(maximum - 32, 32, 1);
    Expect(sources.PendingValue(maximum - 1, 1) == 1, "range near address limit does not wrap");
    completed.store(1, std::memory_order_release);
    completed.notify_all();
    sources.Track(0x1000, 0x1000, 2);
    completed.store(2, std::memory_order_release);
    completed.notify_all();
    Expect(sources.WaitRange(0x1000, 0x1000) == 0, "completed sources need no guard wait");
    sources.Track(0x1000, 0x1000, 3);
    Expect(sources.PendingValue(0x1000, 0x1000) == 3, "same address gets its new producer");
    Expect(sources.PendingValue(0x1000, 0) == 0, "zero-byte write has no dependency");
}
void TestPruneCompaction() {
    std::atomic<uint64_t> completed {0};
    SourceCopyTracker sources(completed);
    // 200 producers, each a page; contiguous ranges of one producer stay one entry.
    for (uint64_t value = 1; value <= 200; value++) {
        sources.Track(0x100000 + value * 0x1000, 0x800, value);
        sources.Track(0x100000 + value * 0x1000 + 0x800, 0x800, value);
    }
    Expect(sources.PendingValue(0x100000 + 7 * 0x1000 + 0x900, 4) == 7, "coalesced run keeps its producer");
    completed.store(150, std::memory_order_release);
    // The next Track prunes (and compacts) the completed prefix.
    sources.Track(0x900000, 0x1000, 201);
    Expect(sources.PendingValue(0x100000 + 100 * 0x1000, 0x1000) == 0, "completed producer is gone");
    Expect(sources.PendingValue(0x100000 + 160 * 0x1000, 0x1000) == 160, "pending producer survives compaction");
    Expect(sources.PendingValue(0x100000 + 150 * 0x1000, 0x2000) == 151, "overlap reports the newest pending producer");
    Expect(sources.PendingValue(0x900000, 1) == 201, "new producer after compaction");
    completed.store(201, std::memory_order_release);
    Expect(sources.IsIdle() && sources.PendingValue(0x900000, 1) == 0, "everything complete");
    sources.Track(0x1000, 0x1000, 202);
    Expect(sources.PendingValue(0x1000, 1) == 202, "storage reused after a full prune");
}
} // namespace

int main() {
    TestOverlapAndCompletion();
    TestNoGlobalWait();
    TestOrderedSourceOverwrite();
    TestBoundaryAndReuse();
    TestCanonicalProjection();
    TestPruneCompaction();
    std::printf("SourceCopyTracker: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
