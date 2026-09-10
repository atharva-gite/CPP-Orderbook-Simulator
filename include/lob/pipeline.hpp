#pragma once

#include "lob/mpsc_queue.hpp"
#include "lob/order_book.hpp"
#include "lob/seqlock.hpp"
#include "lob/types.hpp"
#include "lob/cache_line.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace lob {

enum class IngressType : uint8_t { Limit = 0, Market = 1, Cancel = 2 };

struct IngressCmd {
    IngressType type = IngressType::Limit;
    Side side = Side::Buy;
    price_t price = 0;
    qty_t qty = 1;
    order_id_t cancel_id = 0;
    uint32_t instrument = 0;
    uint64_t seq = 0; // producer-local sequence for loss checks
};

inline constexpr std::size_t kIngressCap = 1 << 16;

// -----------------------------------------------------------------------------
// (a) Single-writer: all OrderBook mutation on the MPSC consumer thread.
//     Other threads may only SeqlockQuote::read() — they never touch the book.
// -----------------------------------------------------------------------------
class SingleWriterPipeline {
public:
    explicit SingleWriterPipeline(size_t book_orders = 1 << 16)
        : book_(book_orders) {}

    bool try_submit(const IngressCmd& cmd) noexcept { return q_.try_push(cmd); }

    // Drain up to max_cmds. Caller must be the unique consumer thread.
    std::size_t drain(std::size_t max_cmds = 256) {
        std::size_t n = 0;
        IngressCmd cmd;
        while (n < max_cmds && q_.try_pop(cmd)) {
            apply(cmd);
            ++n;
        }
        if (n) publish_quote();
        return n;
    }

    const SeqlockQuote& quotes() const noexcept { return quotes_; }
    OrderBook& book() noexcept { return book_; }
    const OrderBook& book() const noexcept { return book_; }
    uint64_t applied() const noexcept { return applied_.load(std::memory_order_relaxed); }

private:
    void apply(const IngressCmd& cmd) {
        switch (cmd.type) {
            case IngressType::Limit:
                book_.add_limit_order(cmd.side, cmd.price, cmd.qty);
                break;
            case IngressType::Market:
                book_.market_order(cmd.side, cmd.qty);
                break;
            case IngressType::Cancel:
                book_.cancel_order(cmd.cancel_id);
                break;
        }
        applied_.fetch_add(1, std::memory_order_relaxed);
    }

    void publish_quote() {
        Quote q;
        if (auto b = book_.best_bid()) {
            q.has_bid = true;
            q.bid = *b;
            q.bid_qty = book_.best_bid_qty();
        }
        if (auto a = book_.best_ask()) {
            q.has_ask = true;
            q.ask = *a;
            q.ask_qty = book_.best_ask_qty();
        }
        quotes_.publish(q);
    }

    MpscRing<IngressCmd, kIngressCap> q_;
    OrderBook book_;
    SeqlockQuote quotes_;
    std::atomic<uint64_t> applied_{0};
};

// -----------------------------------------------------------------------------
// (b) Sharded books: partition by instrument id. Each shard has a std::mutex.
//
//     NOT lock-free. Mutex-per-shard is "less contended" than one global lock
//     when the workload spreads across instruments. Two threads on the same
//     instrument still serialize. Price-range sharding is not used: a market
//     order must walk contiguous levels of one book; splitting one instrument
//     by price would break matching.
// -----------------------------------------------------------------------------
class ShardedBooks {
public:
    explicit ShardedBooks(std::size_t n_shards, size_t book_orders = 1 << 14)
        : shards_(n_shards) {
        for (auto& s : shards_) {
            s = std::make_unique<Shard>(book_orders);
        }
    }

    void submit(const IngressCmd& cmd) {
        Shard& s = *shards_[cmd.instrument % shards_.size()];
        std::lock_guard<std::mutex> lock(s.mu); // blocking lock, not lock-free
        apply(s.book, cmd);
        s.applied.fetch_add(1, std::memory_order_relaxed);
    }

    std::size_t shard_count() const noexcept { return shards_.size(); }

    uint64_t applied() const noexcept {
        uint64_t n = 0;
        for (const auto& s : shards_) n += s->applied.load(std::memory_order_relaxed);
        return n;
    }

    // Snapshot under the shard lock so the caller sees a consistent book.
    Quote quote(uint32_t instrument) const {
        const Shard& s = *shards_[instrument % shards_.size()];
        std::lock_guard<std::mutex> lock(s.mu);
        Quote q;
        if (auto b = s.book.best_bid()) {
            q.has_bid = true;
            q.bid = *b;
            q.bid_qty = s.book.best_bid_qty();
        }
        if (auto a = s.book.best_ask()) {
            q.has_ask = true;
            q.ask = *a;
            q.ask_qty = s.book.best_ask_qty();
        }
        return q;
    }

private:
    struct Shard {
        explicit Shard(size_t n) : book(n) {}
        alignas(kCacheLine) mutable std::mutex mu;
        OrderBook book;
        std::atomic<uint64_t> applied{0};
    };

    static void apply(OrderBook& book, const IngressCmd& cmd) {
        switch (cmd.type) {
            case IngressType::Limit:
                book.add_limit_order(cmd.side, cmd.price, cmd.qty);
                break;
            case IngressType::Market:
                book.market_order(cmd.side, cmd.qty);
                break;
            case IngressType::Cancel:
                book.cancel_order(cmd.cancel_id);
                break;
        }
    }

    std::vector<std::unique_ptr<Shard>> shards_;
};

} // namespace lob
