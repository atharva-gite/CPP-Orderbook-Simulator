#pragma once

#include "lob/cache_line.hpp"
#include "lob/types.hpp"

#include <atomic>
#include <cstdint>

namespace lob {

// Seqlock quote of the inside market.
//
// Honesty:
//   The WRITER never waits on readers (no shared mutex). That is the point.
//   Readers retry while a write is in progress (odd seq) or if seq changed.
//   Reader retry is a SPIN, not lock-free: a continuous writer can starve
//   readers (livelock). Seqlock is "writer-uncontended", not lock-free reads.
//
// TSan:
//   Payload fields are atomics so a concurrent store/load is not a C++ data
//   race. Torn combinations are discarded by the seq check.
struct Quote {
    price_t bid = 0;
    price_t ask = 0;
    qty_t bid_qty = 0;
    qty_t ask_qty = 0;
    bool has_bid = false;
    bool has_ask = false;
};

class SeqlockQuote {
public:
    void publish(const Quote& q) noexcept {
        // Fetch-add relaxed then fence: make seq odd so readers spin.
        // We do not use acq_rel on this first bump — no data is published yet.
        seq_.fetch_add(1, std::memory_order_relaxed);
        // release fence: all payload stores below must not move before the odd seq.
        std::atomic_thread_fence(std::memory_order_release);

        bid_.store(q.bid, std::memory_order_relaxed);
        ask_.store(q.ask, std::memory_order_relaxed);
        bid_qty_.store(q.bid_qty, std::memory_order_relaxed);
        ask_qty_.store(q.ask_qty, std::memory_order_relaxed);
        has_bid_.store(q.has_bid, std::memory_order_relaxed);
        has_ask_.store(q.has_ask, std::memory_order_relaxed);

        // release: publishes payload; readers' acquire load of seq see it.
        seq_.fetch_add(1, std::memory_order_release);
    }

    Quote read() const noexcept {
        Quote q;
        for (;;) {
            // acquire: if seq is even, payload stores from the matching publish
            // are visible. If odd, a write is in flight — retry.
            const uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1ull) continue;

            q.bid = bid_.load(std::memory_order_relaxed);
            q.ask = ask_.load(std::memory_order_relaxed);
            q.bid_qty = bid_qty_.load(std::memory_order_relaxed);
            q.ask_qty = ask_qty_.load(std::memory_order_relaxed);
            q.has_bid = has_bid_.load(std::memory_order_relaxed);
            q.has_ask = has_ask_.load(std::memory_order_relaxed);

            // acquire fence + relaxed seq: catch a writer that started after s1.
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint64_t s2 = seq_.load(std::memory_order_relaxed);
            if (s1 == s2) return q;
        }
    }

    uint64_t seq() const noexcept { return seq_.load(std::memory_order_acquire); }

private:
    alignas(kCacheLine) std::atomic<uint64_t> seq_{0};
    std::atomic<price_t> bid_{0};
    std::atomic<price_t> ask_{0};
    std::atomic<qty_t> bid_qty_{0};
    std::atomic<qty_t> ask_qty_{0};
    std::atomic<bool> has_bid_{false};
    std::atomic<bool> has_ask_{false};
};

} // namespace lob
