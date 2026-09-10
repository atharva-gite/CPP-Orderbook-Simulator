#pragma once

#include "lob/order_book.hpp"
#include <cstdint>
#include <random>
#include <vector>

namespace lob {

// Phase 4: Poisson-style order-flow mix (limit / market / cancel) driven
// into a live OrderBook. Inter-arrival waiting is *not* applied — the
// caller decides whether to sleep. Throughput benches call step() as fast
// as possible to measure max sustained events/sec the book can absorb.
struct SimConfig {
    double lambda_limit = 0.70;
    double lambda_market = 0.15;
    double lambda_cancel = 0.15;
    price_t mid = 10000;
    int tick_span = 128;
    qty_t default_qty = 1;
    size_t target_depth_per_side = 100; // soft cap: extra limits become cancels
};

class OrderFlowSimulator {
public:
    OrderFlowSimulator(uint64_t seed, SimConfig cfg, OrderBook& book);

    // Draw one event from the intensity mix and apply it. Returns false
    // only if the draw was a no-op (cancel on an empty book).
    bool step();

    uint64_t events_applied() const noexcept { return applied_; }
    size_t live_count() const noexcept { return live_.size(); }

private:
    enum class Kind : uint8_t { Limit, Market, Cancel };
    Kind draw_kind();
    void compact_live();
    bool do_limit();
    bool do_market();
    bool do_cancel();

    SimConfig cfg_;
    OrderBook& book_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> u01_{0.0, 1.0};
    std::vector<order_id_t> live_;
    uint64_t applied_ = 0;
    uint64_t steps_since_compact_ = 0;
};

} // namespace lob
