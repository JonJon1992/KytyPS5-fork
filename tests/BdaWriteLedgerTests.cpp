#include "graphics/host_gpu/bdaWriteLedger.h"
#include <array>
#include <cstdio>
#include <span>

using Libs::Graphics::BdaWriteLedger;
using Libs::Graphics::GuestRange;

namespace {
int failures = 0;
void Expect(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "BdaWriteLedger: %s\n", message);
        ++failures;
    }
}
void TestOverlappingWriters() {
    BdaWriteLedger ledger;
    const std::array domain {GuestRange{0x1000, 0x1000}};
    const auto epoch = ledger.UnknownWriteEpoch();
    const auto a = ledger.Open(17, 91, domain);
    const auto b = ledger.Open(18, 91, domain);
    Expect(a != 0 && b > a, "writers get distinct producer tickets");
    Expect(ledger.HasPending() && ledger.UnknownWriteEpoch() != epoch, "unknown write opens an epoch");
    Expect(ledger.PendingForRange(0x1000, 1) == b, "overlap retains the later producer");
    Expect(ledger.Apply(a), "owner applies A once");
    Expect(ledger.PendingForRange(0x1000, 1) == b && ledger.HasPending(), "A cannot release B");
    Expect(!ledger.Apply(a), "duplicate result cannot close B");
    Expect(ledger.AppliedPrefix() == a, "A publication can advance independently of B");
    Expect(ledger.Apply(b) && !ledger.HasPending(), "last application closes the epoch");
    Expect(ledger.UnknownWriteEpoch() != epoch, "certificates predating unknown writes stay invalid");
}
void TestIndependentRangesAndPrefix() {
    BdaWriteLedger ledger;
    const std::array left {GuestRange{0x1000, 0x100}};
    const std::array right {GuestRange{0x3000, 0x100}};
    const auto a = ledger.Open(21, 101, left);
    const auto b = ledger.Open(22, 102, right);
    Expect(ledger.PendingForRange(0x10ff, 1) == a, "reader depends only on its producer");
    Expect(ledger.PendingForRange(0x1100, 1) == 0, "end is exclusive");
    Expect(ledger.PendingForRange(0x2000, 0x100) == 0, "unrelated page does not wait");
    Expect(ledger.Apply(b), "later independent producer can apply first");
    Expect(ledger.AppliedPrefix() == 0, "native or out-of-order completion is not an applied prefix");
    Expect(ledger.PendingForRange(0x3000, 1) == 0 && ledger.PendingForRange(0x1000, 1) == a,
           "applying B releases only B's readers");
    const auto* producer = ledger.Get(a);
    Expect(producer && producer->mapping_generation == 101 && producer->tick == 21,
           "retained producer keeps its mapping generation and tick");
    Expect(ledger.Apply(a) && ledger.AppliedPrefix() == b, "closing the prefix hole releases publications");
}
void TestCapacityAndReuse() {
    BdaWriteLedger ledger;
    const std::array domain {GuestRange{0x1000, 0x1000}, GuestRange{0x8000, 0x1000}};
    std::array<BdaWriteLedger::Ticket, BdaWriteLedger::Capacity> tickets {};
    for (auto& ticket : tickets) ticket = ledger.Open(30, 7, domain);
    Expect(ledger.Open(31, 7, domain) == 0, "full ring refuses admission before a writer");
    Expect(ledger.Apply(tickets.back()), "last result may be collected early");
    Expect(ledger.Open(31, 7, domain) == 0, "retirement waits for the contiguous prefix");
    const auto* first = ledger.Get(tickets.front());
    const auto* storage = first ? first->domain.data() : nullptr;
    for (auto ticket : tickets) (void)ledger.Apply(ticket);
    const auto next = ledger.Open(31, 8, domain);
    const auto* reused = ledger.Get(next);
    Expect(next > tickets.back() && reused && reused->mapping_generation == 8,
           "remapped address receives a fresh producer identity");
    Expect(storage && reused && storage == reused->domain.data(), "ring reuses retained domain capacity");
    Expect(ledger.PendingForRange(0x8000, 0) == 0, "empty read has no dependency");
    Expect(ledger.Apply(next), "reused slot is independently applicable");
}
void TestEmptyDomain() {
    BdaWriteLedger ledger;
    const std::array domain {GuestRange{0x1000, 0x1000}};
    const auto a = ledger.Open(40, 9, std::span<const GuestRange> {});
    const auto b = ledger.Open(41, 9, domain);
    Expect(a != 0 && b > a, "a writer with an empty history is admitted");
    Expect(ledger.PendingForRange(0, UINT64_MAX >> 1) == b, "an empty domain holds no reader");
    Expect(ledger.Apply(b) && ledger.AppliedPrefix() == 0, "an empty producer still gates publications");
    Expect(ledger.Apply(a) && ledger.AppliedPrefix() == b && !ledger.HasPending(),
           "applying the empty producer releases the prefix");
}
}
int main() {
    TestOverlappingWriters();
    TestIndependentRangesAndPrefix();
    TestCapacityAndReuse();
    TestEmptyDomain();
    std::printf("BdaWriteLedger: %d failures\n", failures);
    return failures != 0;
}
