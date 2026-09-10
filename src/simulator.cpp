#include "lob/simulator.hpp"
#include <algorithm>

namespace lob {

OrderFlowSimulator::OrderFlowSimulator(uint64_t seed, SimConfig cfg, OrderBook& book)
    : cfg_(cfg), book_(book), rng_(seed)
{
}

OrderFlowSimulator::Kind OrderFlowSimulator::draw_kind() {
    const double z = cfg_.lambda_limit + cfg_.lambda_market + cfg_.lambda_cancel;
    double u = u01_(rng_) * z;
    if (u < cfg_.lambda_limit) return Kind::Limit;
    u -= cfg_.lambda_limit;
    if (u < cfg_.lambda_market) return Kind::Market;
    return Kind::Cancel;
}

void OrderFlowSimulator::compact_live() {
    live_.erase(std::remove_if(live_.begin(), live_.end(),
                               [&](order_id_t id) { return book_.get_order(id) == nullptr; }),
                live_.end());
    steps_since_compact_ = 0;
}

bool OrderFlowSimulator::do_limit() {
    if (live_.size() >= cfg_.target_depth_per_side * 2 + 64) {
        return do_cancel();
    }
    // Exponential-ish distance from mid: closer ticks more often.
    int span = std::max(1, cfg_.tick_span);
    int mag = 1 + static_cast<int>(u01_(rng_) * u01_(rng_) * static_cast<double>(span));
    bool buy = u01_(rng_) < 0.5;
    price_t px = buy ? (cfg_.mid - mag) : (cfg_.mid + mag);
    auto id = book_.add_limit_order(buy ? Side::Buy : Side::Sell, px, cfg_.default_qty);
    if (id == 0) return false;
    live_.push_back(id);
    return true;
}

bool OrderFlowSimulator::do_market() {
    bool buy = u01_(rng_) < 0.5;
    qty_t q = 1 + static_cast<qty_t>(rng_() % 4);
    auto fills = book_.market_order(buy ? Side::Buy : Side::Sell, q);
    (void)fills;
    return true;
}

bool OrderFlowSimulator::do_cancel() {
    if (live_.empty()) return do_limit();
    for (int attempt = 0; attempt < 8; ++attempt) {
        size_t i = static_cast<size_t>(rng_() % live_.size());
        order_id_t id = live_[i];
        live_[i] = live_.back();
        live_.pop_back();
        if (book_.cancel_order(id)) return true;
    }
    return false;
}

bool OrderFlowSimulator::step() {
    Kind k = draw_kind();
    bool ok = false;
    switch (k) {
        case Kind::Limit:  ok = do_limit(); break;
        case Kind::Market: ok = do_market(); break;
        case Kind::Cancel: ok = do_cancel(); break;
    }
    if (ok) ++applied_;
    if (++steps_since_compact_ >= 4096) compact_live();
    return ok;
}

} // namespace lob
