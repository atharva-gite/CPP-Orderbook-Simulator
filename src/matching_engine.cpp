#include "lob/matching_engine.hpp"
#include "lob/order_pool.hpp"
#include <algorithm>
#include <cassert>

namespace lob {

// FIFO policy: call OrderBook market_order to execute directly for now
std::vector<Fill> FIFOStack::match(OrderBook& book, Side incoming_side, qty_t qty, price_t) {
    return book.market_order(incoming_side, qty);
}

// Pro-rata: at each price level distribute proportionally
std::vector<Fill> ProRataPolicy::match(OrderBook& book, Side incoming_side, qty_t qty, price_t) {
    std::vector<Fill> fills;
    auto best = (incoming_side == Side::Buy) ? book.best_ask() : book.best_bid();
    if (!best) return fills;
    price_t lvl_price = *best;

    // get resting orders at this level in FIFO order
    auto orders = book.get_level_orders(incoming_side == Side::Buy ? Side::Sell : Side::Buy, lvl_price);
    if (orders.empty()) return fills;

    // total available at level
    uint64_t total = 0;
    for (auto* o : orders) total += o->remaining_qty;
    if (total == 0) return fills;

    // deterministic allocation: floor(order_size * qty / total), then distribute remainder
    std::vector<uint64_t> alloc(orders.size());
    std::vector<std::pair<uint64_t, order_id_t>> remainders; // remainder, order_id
    uint64_t sum = 0;
    for (size_t i = 0; i < orders.size(); ++i) {
        uint64_t osz = orders[i]->remaining_qty;
        uint64_t prod = osz * (uint64_t)qty;
        alloc[i] = prod / total;
        uint64_t rem = prod % total;
        remainders.emplace_back(rem, orders[i]->order_id);
        sum += alloc[i];
    }
    uint64_t leftover = (qty > sum) ? (qty - sum) : 0;
    // sort remainders by rem desc, tie by order_id asc for deterministic picks
    std::vector<size_t> idx(orders.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b){
        if (remainders[a].first != remainders[b].first) return remainders[a].first > remainders[b].first;
        return remainders[a].second < remainders[b].second;
    });
    for (size_t k = 0; k < leftover && k < idx.size(); ++k) {
        alloc[idx[k]] += 1;
    }

    // enforce min_fill_: if alloc[i] < min_fill_ then set to 0 (practical policies vary)
    for (size_t i = 0; i < alloc.size(); ++i) {
        if (alloc[i] < min_fill_) alloc[i] = 0;
    }

    // apply allocations in FIFO order and produce fills
    for (size_t i = 0; i < orders.size(); ++i) {
        if (alloc[i] == 0) continue;
        order_id_t id = orders[i]->order_id;
        uint64_t to_take = alloc[i];
        qty_t filled = book.apply_fill(id, static_cast<qty_t>(to_take));
        if (filled > 0) fills.push_back(Fill{id, lvl_price, filled});
    }
    return fills;
}

MatchingEngine::MatchingEngine(OrderBook& book, std::unique_ptr<MatchingPolicy> policy)
    : book_(book), policy_(std::move(policy)) {}

order_id_t MatchingEngine::submit_limit(Side side, price_t price, qty_t qty, qty_t display) {
    order_id_t id = book_.add_limit_order(side, price, qty);
    if (display > 0) {
        // Correct visible/hidden split and price-level aggregate via book helper.
        book_.configure_iceberg(id, display);
    }
    return id;
}

std::vector<Fill> MatchingEngine::submit_market(Side side, qty_t qty) {
    auto fills = policy_->match(book_, side, qty);
    // update last trade price and process triggers if any
    if (!fills.empty()) {
        price_t last_price = fills.back().price;
        process_triggers(last_price);
    }
    return fills;
}

order_id_t MatchingEngine::submit_stop(Side side, price_t stop_price, qty_t qty, StopType stype, price_t limit_price, qty_t display) {
    // Register pending stop via OrderBook helper, and keep pointer locally
    order_id_t id = book_.add_pending_stop(side, stop_price, qty, static_cast<uint8_t>(stype), limit_price, display);
    if (id == 0) return 0;
    Order* o = book_.get_order(id);
    if (o) pending_stops_.push_back(o);
    return id;
}

void MatchingEngine::process_triggers(price_t last_trade_price) {
    // process pending stops in queue order; triggered orders are converted
    // into market/limit submissions and executed after current trade batch
    std::queue<Order*> q;
    // move matching-engine's pending list
    for (auto* o : pending_stops_) q.push(o);
    pending_stops_.clear();
    // also take any pending stops that were registered in the book
    auto from_book = book_.take_pending_stops();
    for (auto* o : from_book) q.push(o);

    while (!q.empty()) {
        Order* o = q.front(); q.pop();
        bool trigger = false;
        Side side = static_cast<Side>(o->side);
            if (o->stop_type == static_cast<uint8_t>(StopType::StopMarket)) {
            if (side == Side::Buy) trigger = (last_trade_price >= o->stop_price);
            else trigger = (last_trade_price <= o->stop_price);
            if (trigger) {
                    // detach pending order from book bookkeeping
                    Order* detached = book_.detach_pending(o->order_id);
                    if (detached) {
                        // execute as market order using its qty
                        qty_t q = detached->qty;
                        book_.free_order(detached);
                        submit_market(side, q);
                    }
            } else {
                // requeue as still pending
                pending_stops_.push_back(o);
            }
        } else if (o->stop_type == static_cast<uint8_t>(StopType::StopLimit)) {
            if (side == Side::Buy) trigger = (last_trade_price >= o->stop_price);
            else trigger = (last_trade_price <= o->stop_price);
            if (trigger) {
                    Order* detached = book_.detach_pending(o->order_id);
                    if (detached) {
                        price_t p = detached->price;
                        qty_t q = detached->qty;
                        qty_t disp = detached->display_qty;
                        book_.free_order(detached);
                        submit_limit(side, p, q, disp);
                    }
            } else {
                pending_stops_.push_back(o);
            }
        }
    }
}

} // namespace lob
