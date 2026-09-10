#include "lob/order_book.hpp"
#include "lob/order_pool.hpp"
#include <sstream>
#include <algorithm>

namespace lob {

// PriceLevel implementation
void PriceLevel::push_back(Order* o) noexcept {
    o->next = nullptr;
    o->prev = tail_;
    if (tail_) tail_->next = o;
    tail_ = o;
    if (!head_) head_ = o;
    ++cnt_;
    agg_qty_ += o->remaining_qty;
}

void PriceLevel::remove(Order* o) noexcept {
    if (o->prev) o->prev->next = o->next;
    else head_ = o->next;
    if (o->next) o->next->prev = o->prev;
    else tail_ = o->prev;
    --cnt_;
    agg_qty_ -= o->remaining_qty;
    o->next = o->prev = nullptr;
}

void PriceLevel::decrease_aggregate(qty_t amt) noexcept {
    if (amt > agg_qty_) agg_qty_ = 0;
    else agg_qty_ -= amt;
}

void PriceLevel::adjust_aggregate(std::int64_t delta) noexcept {
    if (delta >= 0) {
        agg_qty_ += static_cast<qty_t>(delta);
    } else {
        qty_t dec = static_cast<qty_t>(-delta);
        if (dec > agg_qty_) agg_qty_ = 0;
        else agg_qty_ -= dec;
    }
}

// OrderBook implementation
OrderBook::OrderBook(size_t expected_orders, AllocMode alloc)
    : next_order_id_(1), pool_(expected_orders, alloc)
{
    id_slots_.assign(expected_orders * 2 + 8, nullptr);
    bids_.reserve(256);
    asks_.reserve(256);
}

OrderBook::~OrderBook() {
    for (Order* o : id_slots_) {
        if (o) pool_.deallocate(o);
    }
    id_slots_.clear();
    pending_stop_storage_.clear();
}

Order* OrderBook::find_id(order_id_t id) const noexcept {
    if (id >= id_slots_.size()) return nullptr;
    return id_slots_[id];
}

void OrderBook::put_id(Order* o) {
    if (o->order_id >= id_slots_.size()) {
        const std::size_t need = static_cast<std::size_t>(o->order_id) + 1;
        id_slots_.resize(std::max(need, id_slots_.size() * 2), nullptr);
    }
    id_slots_[o->order_id] = o;
}

void OrderBook::drop_id(order_id_t id) noexcept {
    if (id < id_slots_.size()) id_slots_[id] = nullptr;
}

Order* OrderBook::get_order(order_id_t id) noexcept {
    return find_id(id);
}

order_id_t OrderBook::add_pending_stop(Side side, price_t stop_price, qty_t qty, uint8_t stop_type, price_t limit_price, qty_t display) {
    Order* o = pool_.allocate();
    if (!o) return 0;
    o->order_id = next_order_id_++;
    o->side = static_cast<uint8_t>(side);
    o->stop_price = stop_price;
    o->stop_type = stop_type;
    o->price = limit_price;
    o->qty = qty;
    o->remaining_qty = qty;
    o->display_qty = display;
    o->hidden_qty = (display > 0 && qty > display) ? qty - display : 0;
    put_id(o);
    // store as pending by returning id and letting MatchingEngine take via take_pending_stops
    pending_stop_storage_.push_back(o);
    return o->order_id;
}

std::vector<Order*> OrderBook::take_pending_stops() {
    std::vector<Order*> tmp;
    tmp.swap(pending_stop_storage_);
    return tmp;
}

Order* OrderBook::detach_pending(order_id_t id) noexcept {
    Order* found = nullptr;
    // remove from pending storage vector if present
    for (auto it = pending_stop_storage_.begin(); it != pending_stop_storage_.end(); ++it) {
        if ((*it)->order_id == id) {
            found = *it;
            pending_stop_storage_.erase(it);
            break;
        }
    }
    // remove from id table
    Order* mapped = find_id(id);
    if (mapped) {
        if (!found) found = mapped;
        drop_id(id);
    }
    return found;
}

void OrderBook::free_order(Order* o) noexcept {
    if (!o) return;
    pool_.deallocate(o);
}

std::vector<Order*> OrderBook::get_level_orders(Side side, price_t price) const {
    std::vector<Order*> list;
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        if (it == bids_.end()) return list;
        Order* cur = it->level.head();
        while (cur) {
            list.push_back(cur);
            cur = cur->next;
        }
        return list;
    } else {
        auto it = asks_.find(price);
        if (it == asks_.end()) return list;
        Order* cur = it->level.head();
        while (cur) {
            list.push_back(cur);
            cur = cur->next;
        }
        return list;
    }
}

bool OrderBook::try_replenish_iceberg(Order* o, PriceLevel& lvl) noexcept {
    // Caller has already reduced remaining_qty to 0 and adjusted the level
    // aggregate for the filled visible qty. Replenishment peels a new display
    // slice from hidden and requeues at the tail (loses time priority).
    if (o->display_qty == 0 || o->hidden_qty == 0) return false;
    lvl.remove(o); // remaining is 0 => aggregate unchanged here
    qty_t slice = std::min(o->display_qty, o->hidden_qty);
    o->hidden_qty -= slice;
    o->remaining_qty = slice;
    lvl.push_back(o); // push_back adds slice into aggregate
    return true;
}

bool OrderBook::configure_iceberg(order_id_t id, qty_t display) noexcept {
    Order* o = find_id(id);
    if (!o || display == 0) return false;
    qty_t old_rem = o->remaining_qty;
    o->display_qty = display;
    if (o->qty > display) o->hidden_qty = o->qty - display;
    else o->hidden_qty = 0;
    o->remaining_qty = std::min(display, o->qty);
    std::int64_t delta = static_cast<std::int64_t>(o->remaining_qty) - static_cast<std::int64_t>(old_rem);
    if (static_cast<Side>(o->side) == Side::Buy) {
        auto lit = bids_.find(o->price);
        if (lit != bids_.end()) lit->level.adjust_aggregate(delta);
    } else {
        auto lit = asks_.find(o->price);
        if (lit != asks_.end()) lit->level.adjust_aggregate(delta);
    }
    return true;
}

qty_t OrderBook::apply_fill(order_id_t id, qty_t qty) noexcept {
    Order* o = find_id(id);
    if (!o) return 0;
    qty_t filled = std::min(qty, o->remaining_qty);
    o->remaining_qty -= filled;
    // adjust aggregate in level
    PriceLevel* lvl = nullptr;
    if (static_cast<Side>(o->side) == Side::Buy) {
        auto lit = bids_.find(o->price);
        if (lit != bids_.end()) {
            lit->level.adjust_aggregate(-static_cast<std::int64_t>(filled));
            lvl = &lit->level;
        }
    } else {
        auto lit = asks_.find(o->price);
        if (lit != asks_.end()) {
            lit->level.adjust_aggregate(-static_cast<std::int64_t>(filled));
            lvl = &lit->level;
        }
    }
    if (o->remaining_qty == 0 && lvl) {
        if (try_replenish_iceberg(o, *lvl)) {
            return filled;
        }
        // fully exhausted: remove order from level and id_map and free
        lvl->remove(o);
        if (lvl->order_count() == 0) {
            if (static_cast<Side>(o->side) == Side::Buy) bids_.erase_price(o->price);
            else asks_.erase_price(o->price);
        }
        drop_id(id);
        pool_.deallocate(o);
    }
    return filled;
}

order_id_t OrderBook::add_limit_order(Side side, price_t price, qty_t qty) {
    Order* o = pool_.allocate();
    if (!o) return 0;
    o->order_id = next_order_id_++;
    o->price = price;
    o->qty = qty;
    o->remaining_qty = qty;
    o->side = static_cast<uint8_t>(side);
    o->type = static_cast<uint8_t>(OrderType::Limit);
    o->timestamp = 0; // caller may set

    if (side == Side::Buy) {
        auto it = bids_.find_or_emplace(price);
        it->level.push_back(o);
    } else {
        auto it = asks_.find_or_emplace(price);
        it->level.push_back(o);
    }

    put_id(o);
    return o->order_id;
}

bool OrderBook::cancel_order(order_id_t id) noexcept {
    Order* o = find_id(id);
    if (!o) return false;
    if (static_cast<Side>(o->side) == Side::Buy) {
        auto lit = bids_.find(o->price);
        if (lit != bids_.end()) {
            lit->level.remove(o);
            if (lit->level.order_count() == 0) bids_.erase(lit);
        }
    } else {
        auto lit = asks_.find(o->price);
        if (lit != asks_.end()) {
            lit->level.remove(o);
            if (lit->level.order_count() == 0) asks_.erase(lit);
        }
    }
    drop_id(id);
    pool_.deallocate(o);
    return true;
}

bool OrderBook::modify_order(order_id_t id, price_t new_price, qty_t new_qty) {
    Order* o = find_id(id);
    if (!o) return false;
    price_t old_price = o->price;
    if (new_price == old_price) {
        // in-place qty update, keep time priority
        if (static_cast<Side>(o->side) == Side::Buy) {
            auto lit = bids_.find(old_price);
            if (lit != bids_.end()) {
                qty_t old_total = o->qty;
                qty_t old_rem = o->remaining_qty;
                std::int64_t delta_total = static_cast<std::int64_t>(new_qty) - static_cast<std::int64_t>(old_total);
                qty_t new_rem;
                if (delta_total >= 0) new_rem = old_rem + static_cast<qty_t>(delta_total);
                else new_rem = std::min(old_rem, new_qty);
                std::int64_t delta_rem = static_cast<std::int64_t>(new_rem) - static_cast<std::int64_t>(old_rem);
                o->qty = new_qty;
                o->remaining_qty = new_rem;
                lit->level.adjust_aggregate(delta_rem);
            }
        } else {
            auto lit = asks_.find(old_price);
            if (lit != asks_.end()) {
                qty_t old_total = o->qty;
                qty_t old_rem = o->remaining_qty;
                std::int64_t delta_total = static_cast<std::int64_t>(new_qty) - static_cast<std::int64_t>(old_total);
                qty_t new_rem;
                if (delta_total >= 0) new_rem = old_rem + static_cast<qty_t>(delta_total);
                else new_rem = std::min(old_rem, new_qty);
                std::int64_t delta_rem = static_cast<std::int64_t>(new_rem) - static_cast<std::int64_t>(old_rem);
                o->qty = new_qty;
                o->remaining_qty = new_rem;
                lit->level.adjust_aggregate(delta_rem);
            }
        }
        return true;
    } else {
        // price change -> cancel + replace (loses time priority)
        Side side = static_cast<Side>(o->side);
        qty_t qty = new_qty;
        cancel_order(id);
        add_limit_order(side, new_price, qty);
        return true;
    }
}

std::vector<Fill> OrderBook::market_order(Side side, qty_t qty) noexcept {
    std::vector<Fill> fills;
    qty_t to_fill = qty;
    if (side == Side::Buy) {
        auto it = asks_.begin();
        while (to_fill > 0 && it != asks_.end()) {
            PriceLevel& lvl = it->level;
            Order* cur = lvl.head();
            if (!cur) {
                it = asks_.erase(it);
                continue;
            }
            qty_t take = std::min(to_fill, cur->remaining_qty);
            cur->remaining_qty -= take;
            to_fill -= take;
            lvl.decrease_aggregate(take);
            fills.push_back(Fill{cur->order_id, cur->price, take});
            if (cur->remaining_qty == 0) {
                if (!try_replenish_iceberg(cur, lvl)) {
                    drop_id(cur->order_id);
                    lvl.remove(cur);
                    pool_.deallocate(cur);
                }
                // else: new slice at tail; next iteration takes the new head
            }
            if (lvl.order_count() == 0) {
                it = asks_.erase(it);
            }
            // else remain on this price level (may hit replenished icebergs)
        }
    } else {
        auto it = bids_.begin();
        while (to_fill > 0 && it != bids_.end()) {
            PriceLevel& lvl = it->level;
            Order* cur = lvl.head();
            if (!cur) {
                it = bids_.erase(it);
                continue;
            }
            qty_t take = std::min(to_fill, cur->remaining_qty);
            cur->remaining_qty -= take;
            to_fill -= take;
            lvl.decrease_aggregate(take);
            fills.push_back(Fill{cur->order_id, cur->price, take});
            if (cur->remaining_qty == 0) {
                if (!try_replenish_iceberg(cur, lvl)) {
                    drop_id(cur->order_id);
                    lvl.remove(cur);
                    pool_.deallocate(cur);
                }
            }
            if (lvl.order_count() == 0) {
                it = bids_.erase(it);
            }
        }
    }
    return fills;
}

std::optional<price_t> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->price;
}

std::optional<price_t> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->price;
}

qty_t OrderBook::best_bid_qty() const noexcept {
    if (bids_.empty()) return 0;
    return bids_.begin()->level.aggregate_qty();
}

qty_t OrderBook::best_ask_qty() const noexcept {
    if (asks_.empty()) return 0;
    return asks_.begin()->level.aggregate_qty();
}

std::optional<price_t> OrderBook::midprice() const noexcept {
    auto b = best_bid();
    auto a = best_ask();
    if (!b || !a) return std::nullopt;
    return static_cast<price_t>((*b + *a) / 2);
}

std::string OrderBook::snapshot(size_t max_levels) const {
    std::ostringstream ss;
    ss << "ASKS:\n";
    size_t cnt = 0;
    for (auto it = asks_.begin(); it != asks_.end() && cnt < max_levels; ++it, ++cnt) {
        ss << it->price << " qty=" << it->level.aggregate_qty() << " orders=" << it->level.order_count() << "\n";
    }
    ss << "BIDS:\n";
    cnt = 0;
    for (auto it = bids_.begin(); it != bids_.end() && cnt < max_levels; ++it, ++cnt) {
        ss << it->price << " qty=" << it->level.aggregate_qty() << " orders=" << it->level.order_count() << "\n";
    }
    return ss.str();
}

} // namespace lob
