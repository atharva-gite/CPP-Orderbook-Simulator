#include <benchmark/benchmark.h>
#include "latency_ring.hpp"
#include "lob/matching_engine.hpp"
#include "lob/order_book.hpp"
#include "lob/simulator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace lob;

// Clock choice: std::chrono::steady_clock (not rdtsc / CNTVCT).
//
// Justification vs rdtsc:
// - This host is Darwin/Apple Silicon. Intel `rdtsc` is not a portable
//   clock here; ARM's `cntvct_el0` needs a userspace frequency convert
//   (cntfrq_el0) and is still subject to core migration.
// - steady_clock on macOS is CLOCK_UPTIME_RAW / mach_absolute_time:
//   monotonic, nanosecond type, does not jump with wall-clock adjustments.
// - Call overhead is higher than rdtsc (~20–80 ns vs ~5–15 cycles) so
//   sub-50 ns ops sit near the clock floor — flagged in the summary table
//   rather than silently treated as true engine cost.
// - We record the raw interval; we do not subtract overhead (that can
//   clamp p50 to 0 and hide the floor).
using Clock = std::chrono::steady_clock;

static uint64_t ns_between(Clock::time_point a, Clock::time_point b) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

static double calibrate_clock_overhead_ns() {
    LatencyRing<4096> ring;
    for (int i = 0; i < 4096; ++i) {
        auto a = Clock::now();
        auto b = Clock::now();
        ring.push(ns_between(a, b));
    }
    return ring.percentiles().p50;
}

static double g_clock_overhead_ns = 0.0;

struct ResultRow {
    std::string op;
    int depth = 0;
    const char* alloc = "";
    double p50 = 0;
    double p90 = 0;
    double p99 = 0;
    double p999 = 0;
    double ns_op = 0; // Google Benchmark mean CPU ns/op
    double items_per_s = 0;
    std::string note;
};

static std::mutex g_rows_mu;
static std::map<std::tuple<std::string, int, std::string>, ResultRow> g_rows;

static void publish(ResultRow row) {
    // Probe/warmup invocations often finish with an empty ring — skip those.
    if (row.op != "sim_throughput" && row.p50 <= 0.0 && row.p99 <= 0.0) return;
    std::lock_guard<std::mutex> lock(g_rows_mu);
    auto key = std::make_tuple(row.op, row.depth, std::string(row.alloc));
    g_rows[key] = std::move(row);
}

static const char* alloc_name(int mode) {
    return mode == 0 ? "pool" : "heap";
}

static AllocMode alloc_mode(int mode) {
    return mode == 0 ? AllocMode::Pool : AllocMode::Heap;
}

static constexpr size_t kRingN = 1 << 16;
static constexpr int kWarmup = 256;

// One order per distinct tick so |std::map| == depth per side.
// Bids: [mid-depth, mid-1], asks: [mid+1, mid+depth].
static constexpr price_t kMid = 10000;

static size_t pool_cap_for(int depth) {
    return static_cast<size_t>(depth) * 4 + 4096;
}

static void seed_ladder(OrderBook& book, int depth,
                        std::vector<order_id_t>& bids,
                        std::vector<order_id_t>& asks) {
    bids.clear();
    asks.clear();
    bids.reserve(static_cast<size_t>(depth));
    asks.reserve(static_cast<size_t>(depth));
    for (int i = 0; i < depth; ++i) {
        bids.push_back(book.add_limit_order(Side::Buy, kMid - 1 - i, 1));
        asks.push_back(book.add_limit_order(Side::Sell, kMid + 1 + i, 1));
    }
}

static price_t interior_bid_px(int depth) {
    return kMid - 1 - depth / 2;
}

// -----------------------------------------------------------------------------
// Per-op latency vs depth × allocator, with ring-buffer percentiles.
// range(0)=depth, range(1)=alloc (0 pool, 1 heap)
// -----------------------------------------------------------------------------
static void BM_AddLimitLatency(benchmark::State& state) {
    const int depth = static_cast<int>(state.range(0));
    const int am = static_cast<int>(state.range(1));
    OrderBook book(pool_cap_for(depth), alloc_mode(am));
    std::vector<order_id_t> bids, asks;
    seed_ladder(book, depth, bids, asks);
    const price_t px = interior_bid_px(depth);

    LatencyRing<kRingN> ring;
    int skip = kWarmup;

    for (auto _ : state) {
        auto t0 = Clock::now();
        order_id_t id = book.add_limit_order(Side::Buy, px, 1);
        auto t1 = Clock::now();
        benchmark::DoNotOptimize(id);
        state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
        if (skip > 0) {
            --skip;
        } else {
            ring.push(ns_between(t0, t1));
        }
        book.cancel_order(id);
    }

    auto pct = ring.percentiles();
    state.SetItemsProcessed(state.iterations());
    state.counters["p50"] = pct.p50;
    state.counters["p90"] = pct.p90;
    state.counters["p99"] = pct.p99;
    state.counters["p99.9"] = pct.p999;
    state.SetLabel(std::string("add_limit_order/") + alloc_name(am));
    publish({ "add_limit_order", depth, alloc_name(am), pct.p50, pct.p90, pct.p99, pct.p999,
              0.0, 0.0, "" });
}

static void BM_CancelLatency(benchmark::State& state) {
    const int depth = static_cast<int>(state.range(0));
    const int am = static_cast<int>(state.range(1));
    OrderBook book(pool_cap_for(depth), alloc_mode(am));
    std::vector<order_id_t> bids, asks;
    seed_ladder(book, depth, bids, asks);
    const price_t px = interior_bid_px(depth);
    size_t idx = static_cast<size_t>(depth / 2);

    LatencyRing<kRingN> ring;
    int skip = kWarmup;

    for (auto _ : state) {
        order_id_t id = bids[idx];
        auto t0 = Clock::now();
        bool ok = book.cancel_order(id);
        auto t1 = Clock::now();
        benchmark::DoNotOptimize(ok);
        state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
        if (skip > 0) {
            --skip;
        } else {
            ring.push(ns_between(t0, t1));
        }
        bids[idx] = book.add_limit_order(Side::Buy, px, 1);
    }

    auto pct = ring.percentiles();
    state.SetItemsProcessed(state.iterations());
    state.counters["p50"] = pct.p50;
    state.counters["p90"] = pct.p90;
    state.counters["p99"] = pct.p99;
    state.counters["p99.9"] = pct.p999;
    state.SetLabel(std::string("cancel_order/") + alloc_name(am));
    publish({ "cancel_order", depth, alloc_name(am), pct.p50, pct.p90, pct.p99, pct.p999,
              0.0, 0.0, "" });
}

static void BM_MarketLatency(benchmark::State& state) {
    const int depth = static_cast<int>(state.range(0));
    const int am = static_cast<int>(state.range(1));
    OrderBook book(pool_cap_for(depth), alloc_mode(am));
    std::vector<order_id_t> bids, asks;
    seed_ladder(book, depth, bids, asks);

    LatencyRing<kRingN> ring;
    int skip = kWarmup;

    for (auto _ : state) {
        auto t0 = Clock::now();
        auto fills = book.market_order(Side::Buy, 1);
        auto t1 = Clock::now();
        benchmark::DoNotOptimize(fills.data());
        state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
        if (skip > 0) {
            --skip;
        } else {
            ring.push(ns_between(t0, t1));
        }
        if (!fills.empty()) {
            book.add_limit_order(Side::Sell, fills[0].price, fills[0].qty);
        } else {
            book.add_limit_order(Side::Sell, kMid + 1, 1);
        }
    }

    auto pct = ring.percentiles();
    state.SetItemsProcessed(state.iterations());
    state.counters["p50"] = pct.p50;
    state.counters["p90"] = pct.p90;
    state.counters["p99"] = pct.p99;
    state.counters["p99.9"] = pct.p999;
    state.SetLabel(std::string("market_order/") + alloc_name(am));
    publish({ "market_order", depth, alloc_name(am), pct.p50, pct.p90, pct.p99, pct.p999,
              0.0, 0.0, "" });
}

// Phase 4 simulator → book, as fast as the engine will take events.
static void BM_SimThroughput(benchmark::State& state) {
    const int depth = static_cast<int>(state.range(0));
    const int am = static_cast<int>(state.range(1));
    OrderBook book(pool_cap_for(std::max(depth, 128)), alloc_mode(am));
    std::vector<order_id_t> bids, asks;
    seed_ladder(book, depth, bids, asks);

    SimConfig cfg;
    cfg.target_depth_per_side = static_cast<size_t>(depth);
    cfg.tick_span = std::max(8, depth / 4);
    OrderFlowSimulator sim(0xC0FFEEULL + static_cast<uint64_t>(depth), cfg, book);

    uint64_t n = 0;
    for (auto _ : state) {
        bool ok = sim.step();
        benchmark::DoNotOptimize(ok);
        ++n;
    }
    state.SetItemsProcessed(static_cast<int64_t>(n));
    state.SetLabel(std::string("sim_throughput/") + alloc_name(am));
    publish({ "sim_throughput", depth, alloc_name(am), 0, 0, 0, 0, 0.0, 0.0, "" });
}

static void DepthAllocArgs(benchmark::internal::Benchmark* b) {
    for (int depth : {10, 100, 1000, 10000}) {
        b->Args({depth, 0});
        b->Args({depth, 1});
    }
    b->Unit(benchmark::kNanosecond);
}

static void DepthAllocArgsManual(benchmark::internal::Benchmark* b) {
    DepthAllocArgs(b);
    b->UseManualTime();
}

BENCHMARK(BM_AddLimitLatency)->Apply(DepthAllocArgsManual);
BENCHMARK(BM_CancelLatency)->Apply(DepthAllocArgsManual);
BENCHMARK(BM_MarketLatency)->Apply(DepthAllocArgsManual);
BENCHMARK(BM_SimThroughput)->Apply(DepthAllocArgs)->Unit(benchmark::kNanosecond);

// Keep Phase 3 matching benches (not part of the depth table).
static constexpr size_t kPool = 1 << 18;

static void BM_ProRataMatch(benchmark::State& state) {
    const int n_resting = static_cast<int>(state.range(0));
    OrderBook book(kPool);
    auto policy = std::make_unique<ProRataPolicy>(1);
    MatchingEngine me(book, std::move(policy));
    const qty_t unit = 10;
    const qty_t incoming = static_cast<qty_t>(n_resting) * unit / 2;
    for (auto _ : state) {
        state.PauseTiming();
        for (int i = 0; i < n_resting; ++i) {
            me.submit_limit(Side::Sell, 100, unit * static_cast<qty_t>(1 + (i % 4)));
        }
        state.ResumeTiming();
        auto fills = me.submit_market(Side::Buy, incoming);
        benchmark::DoNotOptimize(fills.data());
        state.PauseTiming();
        me.submit_market(Side::Buy, static_cast<qty_t>(n_resting) * unit * 4);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("ProRataPolicy::match");
}
BENCHMARK(BM_ProRataMatch)->Arg(8)->Arg(32)->Arg(128)->Unit(benchmark::kNanosecond);

static void BM_IcebergReplenish(benchmark::State& state) {
    const qty_t display = static_cast<qty_t>(state.range(0));
    const qty_t total = display * 9;
    OrderBook book(kPool);
    MatchingEngine me(book);
    for (auto _ : state) {
        state.PauseTiming();
        me.submit_limit(Side::Sell, 100, total, display);
        me.submit_limit(Side::Sell, 100, 50, 0);
        state.ResumeTiming();
        auto fills = me.submit_market(Side::Buy, display);
        benchmark::DoNotOptimize(fills.data());
        state.PauseTiming();
        me.submit_market(Side::Buy, total + 50);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("iceberg replenish");
}
BENCHMARK(BM_IcebergReplenish)->Arg(5)->Arg(16)->Arg(64)->Unit(benchmark::kNanosecond);

static const char* op_from_bm_name(const std::string& name) {
    if (name.find("BM_AddLimitLatency") != std::string::npos) return "add_limit_order";
    if (name.find("BM_CancelLatency") != std::string::npos) return "cancel_order";
    if (name.find("BM_MarketLatency") != std::string::npos) return "market_order";
    if (name.find("BM_SimThroughput") != std::string::npos) return "sim_throughput";
    return nullptr;
}

class TableReporter : public benchmark::ConsoleReporter {
public:
    void ReportRuns(const std::vector<Run>& reports) override {
        ConsoleReporter::ReportRuns(reports);
        for (const Run& run : reports) {
            if (run.skipped != benchmark::internal::NotSkipped) continue;
            if (run.run_type != Run::RT_Iteration) continue;
            const char* op = op_from_bm_name(run.benchmark_name());
            if (!op) continue;
            const int depth = static_cast<int>(run.run_name.args.empty() ? 0 : 0);
            // args is "10/0" — parse from benchmark_name after the function.
            int d = 0, am = 0;
            const std::string name = run.benchmark_name();
            auto slash = name.find('/');
            if (slash != std::string::npos) {
                std::sscanf(name.c_str() + static_cast<int>(slash) + 1, "%d/%d", &d, &am);
            }
            (void)depth;
            const double ns = run.GetAdjustedRealTime() > 0 ? run.GetAdjustedRealTime()
                                                            : run.GetAdjustedCPUTime();
            double ips = 0;
            auto cit = run.counters.find("items_per_second");
            if (cit != run.counters.end()) ips = cit->second.value;

            std::lock_guard<std::mutex> lock(g_rows_mu);
            auto key = std::make_tuple(std::string(op), d, std::string(alloc_name(am)));
            auto it = g_rows.find(key);
            if (it == g_rows.end()) continue;
            it->second.ns_op = ns;
            it->second.items_per_s = ips;
        }
    }
};

static std::vector<ResultRow> snapshot_rows() {
    std::vector<ResultRow> rows;
    std::lock_guard<std::mutex> lock(g_rows_mu);
    rows.reserve(g_rows.size());
    for (auto& kv : g_rows) rows.push_back(kv.second);
    std::sort(rows.begin(), rows.end(), [](const ResultRow& a, const ResultRow& b) {
        if (a.op != b.op) return a.op < b.op;
        if (std::string(a.alloc) != std::string(b.alloc))
            return std::string(a.alloc) < std::string(b.alloc);
        return a.depth < b.depth;
    });
    return rows;
}

static std::string flag_anomalies() {
    std::ostringstream oss;
    auto rows = snapshot_rows();

    for (size_t i = 1; i < rows.size(); ++i) {
        const auto& prev = rows[i - 1];
        const auto& cur = rows[i];
        if (cur.op != prev.op || std::string(cur.alloc) != std::string(prev.alloc)) continue;
        if (cur.op == "sim_throughput") continue;
        if (cur.p50 <= 0 || prev.p50 <= 0) continue;
        if (cur.depth > prev.depth && cur.p50 < prev.p50 * 0.80) {
            oss << "ANOMALY: " << cur.op << "/" << cur.alloc
                << " p50 dropped " << prev.p50 << "ns @depth " << prev.depth
                << " -> " << cur.p50 << "ns @depth " << cur.depth
                << " (non-monotonic vs map size; likely noise/clock floor)\n";
        }
        if (cur.depth > prev.depth && cur.ns_op > 0 && prev.ns_op > 0 &&
            cur.ns_op < prev.ns_op * 0.80) {
            oss << "ANOMALY: " << cur.op << "/" << cur.alloc
                << " GB ns/op dropped " << prev.ns_op << " @depth " << prev.depth
                << " -> " << cur.ns_op << " @depth " << cur.depth << "\n";
        }
    }
    for (const auto& r : rows) {
        if (r.op == "sim_throughput") continue;
        if (r.p50 > 0 && r.p50 <= 2.0 * g_clock_overhead_ns) {
            oss << "ANOMALY: " << r.op << "/" << r.alloc << " depth=" << r.depth
                << " p50=" << r.p50 << "ns is within 2x clock overhead ("
                << g_clock_overhead_ns << "ns); treat as a floor, not engine time.\n";
        }
        if (r.p999 + 1e-9 < r.p99) {
            oss << "ANOMALY: " << r.op << "/" << r.alloc << " depth=" << r.depth
                << " p99.9 < p99\n";
        }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = i + 1; j < rows.size(); ++j) {
            if (rows[i].op != rows[j].op || rows[i].depth != rows[j].depth) continue;
            if (rows[i].op == "sim_throughput") continue;
            const ResultRow* pool = nullptr;
            const ResultRow* heap = nullptr;
            if (std::string(rows[i].alloc) == "pool") pool = &rows[i];
            if (std::string(rows[j].alloc) == "pool") pool = &rows[j];
            if (std::string(rows[i].alloc) == "heap") heap = &rows[i];
            if (std::string(rows[j].alloc) == "heap") heap = &rows[j];
            if (!pool || !heap || pool->p50 <= 0 || heap->p50 <= 0) continue;
            if (pool->p50 > heap->p50 * 2.0) {
                oss << "ANOMALY: " << pool->op << " depth=" << pool->depth
                    << " pool p50 " << pool->p50 << "ns is >2x heap "
                    << heap->p50 << "ns (allocator win not visible; map node "
                       "alloc or clock floor may dominate).\n";
            }
        }
    }
    return oss.str();
}

static void print_summary_table() {
    auto rows = snapshot_rows();

    std::cout << "\n";
    std::cout << "Clock: std::chrono::steady_clock  |  overhead p50 = "
              << g_clock_overhead_ns << " ns (empty now()-now())\n";
    std::cout << "Depth = resting orders per side, 1 order / distinct tick "
                 "(std::map size == depth).\n";
    std::cout << "market_order times 1-lot at the inside; std::map::begin() is "
                 "O(1) so this path should stay ~flat vs depth.\n";
    std::cout << "add/cancel hit an interior tick so map find is O(log depth).\n";
    std::cout << "GB ns/op for latency benches is UseManualTime of the same "
                 "chrono interval as the histogram (restore is untimed).\n";
    std::cout << "\n";
    std::cout << std::left
              << std::setw(18) << "op" << std::setw(8) << "alloc"
              << std::setw(8) << "depth" << std::setw(10) << "p50"
              << std::setw(10) << "p90" << std::setw(10) << "p99"
              << std::setw(10) << "p99.9" << std::setw(12) << "ns/op"
              << std::setw(14) << "events/s" << "\n";
    std::cout << std::string(100, '-') << "\n";
    std::cout << std::fixed << std::setprecision(1);
    for (const auto& r : rows) {
        std::cout << std::left << std::setw(18) << r.op << std::setw(8) << r.alloc
                  << std::setw(8) << r.depth << std::setw(10) << r.p50
                  << std::setw(10) << r.p90 << std::setw(10) << r.p99
                  << std::setw(10) << r.p999 << std::setw(12) << r.ns_op;
        if (r.op == "sim_throughput") {
            std::cout << std::setprecision(0) << std::setw(14) << r.items_per_s
                      << std::setprecision(1);
        } else {
            std::cout << std::setw(14) << "-";
        }
        std::cout << "\n";
    }
    std::cout << "\n";
    std::string flags = flag_anomalies();
    if (flags.empty()) {
        std::cout << "Anomaly flags: none (p50 non-decreasing within 20% across "
                     "depth, p99.9>=p99, pool not >2x heap on add/cancel).\n";
    } else {
        std::cout << flags;
    }
}

int main(int argc, char** argv) {
    g_clock_overhead_ns = calibrate_clock_overhead_ns();
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    TableReporter reporter;
    benchmark::RunSpecifiedBenchmarks(&reporter);
    print_summary_table();
    benchmark::Shutdown();
    return 0;
}
