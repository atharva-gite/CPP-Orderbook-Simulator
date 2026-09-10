#pragma once

#include "lob/order_book.hpp"
#include <queue>
#include <vector>
#include <memory>

namespace lob {

enum class StopType : uint8_t { None = 0, StopMarket = 1, StopLimit = 2 };

// MatchingPolicy interface — default FIFO policy implemented inline
struct MatchingPolicy {
    virtual ~MatchingPolicy() = default;
    virtual std::vector<Fill> match(OrderBook& book, Side incoming_side, qty_t qty, price_t taker_price = 0) = 0;
};

// FIFO matching policy (time-priority)
struct FIFOStack : MatchingPolicy {
    std::vector<Fill> match(OrderBook& book, Side incoming_side, qty_t qty, price_t) override;
};

// Pro-rata matching policy
struct ProRataPolicy : MatchingPolicy {
    // min_fill: minimum allocation per resting order, remainder_rule: 0=by order_id asc
    explicit ProRataPolicy(qty_t min_fill = 1) : min_fill_(min_fill) {}
    std::vector<Fill> match(OrderBook& book, Side incoming_side, qty_t qty, price_t) override;
private:
    qty_t min_fill_;
};

class MatchingEngine {
public:
    explicit MatchingEngine(OrderBook& book, std::unique_ptr<MatchingPolicy> policy = std::make_unique<FIFOStack>());

    // Entry points
    order_id_t submit_limit(Side side, price_t price, qty_t qty, qty_t display = 0);
    std::vector<Fill> submit_market(Side side, qty_t qty);

    // Stop order submission
    order_id_t submit_stop(Side side, price_t stop_price, qty_t qty, StopType stype, price_t limit_price = 0, qty_t display = 0);

private:
    OrderBook& book_;
    std::unique_ptr<MatchingPolicy> policy_;

    // pending stop orders
    std::vector<Order*> pending_stops_;

    // process triggers from queue to avoid recursion
    void process_triggers(price_t last_trade_price);
};

} // namespace lob
