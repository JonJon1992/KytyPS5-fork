#pragma once

#include "graphics/host_gpu/regionDefinitions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <span>
#include <vector>

namespace Libs::Graphics {

// Domains and producer state belong to the GPU recording thread. Only the
// completion prefix, pending hint and epoch are published to foreign readers.
class BdaWriteLedger {
public:
    static constexpr size_t Capacity = 8;
    using Ticket = uint64_t;
    struct Producer {
        Ticket ticket = 0;
        uint64_t tick = 0;
        uint64_t mapping_generation = 0;
        std::vector<GuestRange> domain;
        bool applied = true;
    };

    // Admission fails before recording if retained slots are full or the domain
    // is not a sorted, disjoint set. An empty domain (a writer whose history holds
    // no page) holds no reader, only publications. No allocation on range queries.
    Ticket Open(uint64_t tick, uint64_t generation, std::span<const GuestRange> domain) {
        if (tick == 0 || m_last - AppliedPrefix() == Capacity ||
            m_last == std::numeric_limits<Ticket>::max()) return 0;
        uint64_t end = 0;
        for (const auto range : domain) {
            if (range.size == 0 || range.size > UINT64_MAX - range.address ||
                range.address < end) return 0;
            end = range.End();
        }
        const auto ticket = ++m_last;
        auto& producer = m_producers[(ticket - 1) % Capacity];
        producer.domain.assign(domain.begin(), domain.end()); // capacity retained
        producer.ticket = ticket;
        producer.tick = tick;
        producer.mapping_generation = generation;
        producer.applied = false;
        m_epoch.fetch_add(1, std::memory_order_release);
        m_pending.fetch_add(1, std::memory_order_release);
        return ticket;
    }

    // An out-of-order application releases its readers, but publication advances
    // only across a contiguous applied prefix. Duplicate results cannot close a
    // reused slot or another producer on the same page.
    bool Apply(Ticket ticket) {
        auto* producer = const_cast<Producer*>(Get(ticket));
        if (producer == nullptr || producer->applied) return false;
        producer->applied = true;
        m_epoch.fetch_add(1, std::memory_order_release);
        m_pending.fetch_sub(1, std::memory_order_release);
        auto prefix = AppliedPrefix();
        while (prefix < m_last && m_producers[prefix % Capacity].applied) ++prefix;
        m_applied.store(prefix, std::memory_order_release);
        m_applied.notify_all();
        return true;
    }

    Ticket PendingForRange(uint64_t address, uint64_t size) const {
        if (size == 0 || size > UINT64_MAX - address || !HasPending()) return 0;
        const auto end = address + size;
        Ticket pending = 0;
        for (const auto& producer : m_producers) {
            if (producer.applied) continue;
            const auto it = std::lower_bound(producer.domain.begin(), producer.domain.end(), address,
                [](GuestRange range, uint64_t begin) { return range.End() <= begin; });
            if (it != producer.domain.end() && it->address < end)
                pending = std::max(pending, producer.ticket);
        }
        return pending;
    }
    const Producer* Get(Ticket ticket) const {
        if (ticket == 0 || ticket > m_last || m_last - ticket >= Capacity) return nullptr;
        const auto& producer = m_producers[(ticket - 1) % Capacity];
        return producer.ticket == ticket ? &producer : nullptr;
    }
    Ticket LastTicket() const { return m_last; }
    Ticket AppliedPrefix() const { return m_applied.load(std::memory_order_acquire); }
    bool HasPending() const { return m_pending.load(std::memory_order_acquire) != 0; }
    uint64_t UnknownWriteEpoch() const { return m_epoch.load(std::memory_order_acquire); }

private:
    std::array<Producer, Capacity> m_producers;
    Ticket m_last = 0;
    std::atomic<Ticket> m_applied {0};
    std::atomic<uint32_t> m_pending {0};
    std::atomic<uint64_t> m_epoch {1};
};
} // namespace Libs::Graphics
