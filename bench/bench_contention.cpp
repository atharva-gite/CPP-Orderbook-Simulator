// Contention bench: N producers submitting as fast as possible.
//
// Standalone (no Google Benchmark) so the same binary can run under TSan.
//
// Honesty: print hardware_concurrency() and treat N > cores as
// oversubscription, not a scaling result. Lost-update checks are the
// correctness deliverable.

#include "lob/pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

using namespace lob;
using Clock = std::chrono::steady_clock;

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LOB_TSAN 1
#endif
#endif
#ifdef THREAD_SANITIZER
#define LOB_TSAN 1
#endif

#ifdef LOB_TSAN
static constexpr int kOpsEach = 3000;
#else
static constexpr int kOpsEach = 40000;
#endif

static uint64_t ns_between(Clock::time_point a, Clock::time_point b) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

static double pct(std::vector<uint64_t>& v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double idx = q * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return static_cast<double>(v[lo]) * (1.0 - frac) + static_cast<double>(v[hi]) * frac;
}

struct Row {
    const char* model;
    int nprod;
    double ingest_per_s;
    double p50_ns;
    double p99_ns;
    uint64_t applied;
};

static IngressCmd make_cmd(int prod, int i, int n_instruments) {
    IngressCmd c;
    c.type = (i % 17 == 0) ? IngressType::Market : IngressType::Limit;
    c.side = (i & 1) ? Side::Buy : Side::Sell;
    c.price = 10000 + static_cast<price_t>((i % 40) - 20);
    c.qty = 1;
    c.instrument = static_cast<uint32_t>((prod + i) % std::max(1, n_instruments));
    c.seq = (static_cast<uint64_t>(prod) << 32) | static_cast<uint32_t>(i);
    return c;
}

static Row run_single_writer(int nprod, int ops_each) {
    SingleWriterPipeline pipe(1 << 16);
    std::atomic<bool> run_cons{true};
    std::thread consumer([&] {
        while (run_cons.load(std::memory_order_acquire) ||
               pipe.applied() < static_cast<uint64_t>(nprod) * static_cast<uint64_t>(ops_each)) {
            if (pipe.drain(128) == 0) std::this_thread::yield();
        }
    });

    std::vector<std::vector<uint64_t>> samples(static_cast<size_t>(nprod));
    std::vector<std::thread> producers;
    producers.reserve(static_cast<size_t>(nprod));
    const auto t0 = Clock::now();
    for (int p = 0; p < nprod; ++p) {
        producers.emplace_back([&, p] {
            auto& mine = samples[static_cast<size_t>(p)];
            mine.reserve(static_cast<size_t>(ops_each));
            for (int i = 0; i < ops_each; ++i) {
                IngressCmd c = make_cmd(p, i, 1);
                auto a = Clock::now();
                while (!pipe.try_submit(c)) {
                    // Full ring: caller spin. Not lock-free progress for this thread.
                }
                auto b = Clock::now();
                mine.push_back(ns_between(a, b));
            }
        });
    }
    for (auto& t : producers) t.join();
    const auto t1 = Clock::now();
    while (pipe.applied() < static_cast<uint64_t>(nprod) * static_cast<uint64_t>(ops_each)) {
        std::this_thread::yield();
    }
    run_cons.store(false, std::memory_order_release);
    consumer.join();

    std::vector<uint64_t> all;
    for (auto& s : samples) all.insert(all.end(), s.begin(), s.end());
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    const uint64_t total = static_cast<uint64_t>(nprod) * static_cast<uint64_t>(ops_each);
    return {"single-writer+MPSC", nprod, static_cast<double>(total) / std::max(sec, 1e-9),
            pct(all, 0.50), pct(all, 0.99), pipe.applied()};
}

static Row run_sharded(int nprod, int ops_each, int nshards) {
    ShardedBooks books(static_cast<size_t>(nshards), 1 << 14);
    std::vector<std::vector<uint64_t>> samples(static_cast<size_t>(nprod));
    std::vector<std::thread> producers;
    const auto t0 = Clock::now();
    for (int p = 0; p < nprod; ++p) {
        producers.emplace_back([&, p] {
            auto& mine = samples[static_cast<size_t>(p)];
            mine.reserve(static_cast<size_t>(ops_each));
            for (int i = 0; i < ops_each; ++i) {
                IngressCmd c = make_cmd(p, i, nshards);
                auto a = Clock::now();
                books.submit(c); // std::mutex — blocking, not lock-free
                auto b = Clock::now();
                mine.push_back(ns_between(a, b));
            }
        });
    }
    for (auto& t : producers) t.join();
    const auto t1 = Clock::now();
    std::vector<uint64_t> all;
    for (auto& s : samples) all.insert(all.end(), s.begin(), s.end());
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    const uint64_t total = static_cast<uint64_t>(nprod) * static_cast<uint64_t>(ops_each);
    return {"sharded-mutex", nprod, static_cast<double>(total) / std::max(sec, 1e-9),
            pct(all, 0.50), pct(all, 0.99), books.applied()};
}

int main() {
    const unsigned hw = std::thread::hardware_concurrency();
    std::cout << "hardware_concurrency() = " << hw << "\n";
    std::cout << "ops per producer = " << kOpsEach << "\n";
#ifdef LOB_TSAN
    std::cout << "TSan instrumentation: ON (ops reduced)\n";
#endif
    std::cout << "\nCAVEAT: ingest/s vs N is only meaningful up to ~physical cores. "
                 "N > cores is oversubscription. Do not treat this table as a "
                 "scaling study unless the machine has that many cores.\n\n";

    const int max_n = static_cast<int>(hw == 0 ? 2 : hw);
    std::vector<int> ns;
    for (int n = 1; n <= max_n; n *= 2) ns.push_back(n);
    if (ns.back() != max_n) ns.push_back(max_n);

    std::vector<Row> rows;
    for (int n : ns) {
        rows.push_back(run_single_writer(n, kOpsEach));
        rows.push_back(run_sharded(n, kOpsEach, std::max(1, n)));
    }

    std::cout << std::left << std::setw(22) << "model" << std::setw(6) << "N"
              << std::setw(14) << "ingest/s" << std::setw(12) << "p50_ns"
              << std::setw(12) << "p99_ns" << std::setw(12) << "applied"
              << "\n";
    std::cout << std::string(78, '-') << "\n";
    std::cout << std::fixed << std::setprecision(0);
    int rc = 0;
    for (const auto& r : rows) {
        std::cout << std::left << std::setw(22) << r.model << std::setw(6) << r.nprod
                  << std::setw(14) << r.ingest_per_s << std::setw(12) << r.p50_ns
                  << std::setw(12) << r.p99_ns << std::setw(12) << r.applied << "\n";
        const uint64_t expect = static_cast<uint64_t>(r.nprod) * static_cast<uint64_t>(kOpsEach);
        if (r.applied != expect) {
            std::cerr << "LOST UPDATES model=" << r.model << " N=" << r.nprod
                      << " applied=" << r.applied << " expected=" << expect << "\n";
            rc = 1;
        }
    }
    return rc;
}
