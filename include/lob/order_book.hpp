#pragma once

#include "lob/order.hpp"
#include "lob/types.hpp"
#include <functional>
#include <optional>
#include <vector>
#include <cstdint>
#include <string>
#include "lob/order_pool.hpp"

namespace lob {

struct Fill {
    order_id_t resting_order_id;
    price_t price;
    qty_t qty;
};

// Intrusive price level: FIFO doubly-linked list of Order*
class PriceLevel {
public:
    PriceLevel() noexcept = default;

    void push_back(Order* o) noexcept;
    void remove(Order* o) noexcept;
    void decrease_aggregate(qty_t amt) noexcept;
    void adjust_aggregate(std::int64_t delta) noexcept;

    qty_t aggregate_qty() const noexcept { return agg_qty_; }
    size_t order_count() const noexcept { return cnt_; }

    Order* head() const noexcept { return head_; }

private:
    Order* head_ = nullptr;
    Order* tail_ = nullptr;
    qty_t agg_qty_ = 0;
    size_t cnt_ = 0;
};

// Comparator for bids: descending order
struct BidCmp {
    bool operator()(const price_t& a, const price_t& b) const noexcept {
        return a > b;
    }
};

} // namespace lob

#include "lob/price_ladder.hpp"

namespace lob {

class OrderBook {
public:
    explicit OrderBook(size_t expected_orders = 1024, AllocMode alloc = AllocMode::Pool);
    ~OrderBook();

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // Add a limit order, returns order id (0 on failure)
    order_id_t add_limit_order(Side side, price_t price, qty_t qty);

    // Cancel by id
    bool cancel_order(order_id_t id) noexcept;

    // Modify: if price unchanged -> in-place qty update (keeps time priority)
    // if price changed -> cancel+replace (loses time priority; new order id issued)
    // returns true if modification succeeded. Note: price-change creates a new
    // order id; the old id is removed. Caller should re-query if it needs the
    // new id.
    bool modify_order(order_id_t id, price_t new_price, qty_t new_qty);

    // Market order: consume liquidity from opposite side, return fills
    std::vector<Fill> market_order(Side side, qty_t qty) noexcept;

    std::optional<price_t> best_bid() const noexcept;
    std::optional<price_t> best_ask() const noexcept;
    std::optional<price_t> midprice() const noexcept;

    // Aggregate visible qty at the inside. 0 if that side is empty.
    qty_t best_bid_qty() const noexcept;
    qty_t best_ask_qty() const noexcept;

    // Snapshot for debugging
    std::string snapshot(size_t max_levels = 10) const;

    // Accessor for external code to find an Order by id (or nullptr)
    Order* get_order(order_id_t id) noexcept;

    // Register a pending stop order; returns order id
    order_id_t add_pending_stop(Side side, price_t stop_price, qty_t qty, uint8_t stop_type, price_t limit_price, qty_t display);
    // Retrieve pending stops (used by MatchingEngine)
    std::vector<Order*> take_pending_stops();
    // Detach a pending order from pending storage and id map; returns pointer (caller owns pointer until freed)
    Order* detach_pending(order_id_t id) noexcept;
    // Free an Order previously detached (returns to pool)
    void free_order(Order* o) noexcept;
    // Return list of orders at a given price level in FIFO order
    std::vector<Order*> get_level_orders(Side side, price_t price) const;
    // Apply a fill to an order by id; returns actual filled qty (may be less if order had less remaining)
    qty_t apply_fill(order_id_t id, qty_t qty) noexcept;

    // Configure an already-resting limit as an iceberg: visible remaining becomes
    // min(display, qty), remainder goes to hidden_qty, and the price-level
    // aggregate is corrected. Returns false if id is unknown.
    bool configure_iceberg(order_id_t id, qty_t display) noexcept;

private:
    // If display exhausted but hidden remains: peel a new slice, move to back of
    // the level (lose time priority). Caller must have remaining_qty == 0.
    // Returns true if replenished (order stays in book); false => fully done.
    bool try_replenish_iceberg(Order* o, PriceLevel& lvl) noexcept;

    Order* find_id(order_id_t id) const noexcept;
    void put_id(Order* o);
    void drop_id(order_id_t id) noexcept;

    PriceLadder<BidCmp> bids_;
    PriceLadder<std::less<price_t>> asks_;

    // Dense id table: order_id is monotonic, so a vector beats node-based
    // unordered_map (sample: tiny_malloc on every emplace).
    std::vector<Order*> id_slots_;
    size_t next_order_id_ = 1;
    OrderPool pool_;
    std::vector<Order*> pending_stop_storage_;
};

} // namespace lob
