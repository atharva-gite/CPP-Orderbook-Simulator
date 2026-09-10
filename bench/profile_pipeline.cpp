// Timed pipeline + structure microbenches for docs/profiling.md.
// Not Google Benchmark: a single deterministic driver so cachegrind/sample
// can wrap one process.

#include "lob/matching_engine.hpp"
#include "lob/order_book.hpp"
#include "lob/simulator.hpp"
#include "md_publisher.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <list>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;

static uint64_t ns_now() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<ns>(Clock::now().time_since_epoch()).count());
}

struct Acc {
    uint64_t n = 0;
    uint64_t ns_sum = 0;
};

static void report(const char* name, const Acc& a) {
    const double mean = a.n ? static_cast<double>(a.ns_sum) / static_cast<double>(a.n) : 0;
    std::printf("  %-28s  n=%-8llu  mean=%.1f ns  total=%.3f ms\n", name,
                static_cast<unsigned long long>(a.n), mean,
                static_cast<double>(a.ns_sum) / 1e6);
}

// --- std::map vs sorted vector at book-like depths (isolated from Order) ---
struct BidGt {
    bool operator()(int64_t a, int64_t b) const noexcept { return a > b; }
};

static void bench_map_vs_vector(int depth, int lookups) {
    std::map<int64_t, int, BidGt> tree;
    std::vector<int64_t> vec;
    vec.reserve(static_cast<size_t>(depth));
    for (int i = 0; i < depth; ++i) {
        int64_t px = 10000 - i;
        tree.emplace(px, i);
        vec.push_back(px);
    }
    // vec is already descending

    std::mt19937 rng(1);
    std::uniform_int_distribution<int> dist(0, depth - 1);
    std::vector<int64_t> keys(static_cast<size_t>(lookups));
    for (int i = 0; i < lookups; ++i) keys[static_cast<size_t>(i)] = 10000 - dist(rng);

    volatile int sink = 0;

    auto t0 = Clock::now();
    for (int64_t k : keys) {
        auto it = tree.find(k);
        if (it != tree.end()) sink += it->second;
    }
    auto t1 = Clock::now();
    const double map_ns =
        static_cast<double>(std::chrono::duration_cast<ns>(t1 - t0).count()) /
        static_cast<double>(lookups);

    t0 = Clock::now();
    for (int64_t k : keys) {
        auto it = std::lower_bound(vec.begin(), vec.end(), k, BidGt{});
        if (it != vec.end() && *it == k) sink += static_cast<int>(*it);
    }
    t1 = Clock::now();
    const double vec_ns =
        static_cast<double>(std::chrono::duration_cast<ns>(t1 - t0).count()) /
        static_cast<double>(lookups);

    std::printf("map_vs_vec depth=%d lookups=%d  std::map_find=%.2f ns  "
                "vector_lower_bound=%.2f ns  sink=%d\n",
                depth, lookups, map_ns, vec_ns, sink);
}

// Pointer-chase: pool-stride Order list vs scattered heap Orders.
static void bench_order_walk(int n_orders, int walks) {
    lob::OrderPool pool(static_cast<size_t>(n_orders) + 8, lob::AllocMode::Pool);
    lob::OrderPool heap(static_cast<size_t>(n_orders) + 8, lob::AllocMode::Heap);
    std::vector<lob::Order*> pchain, hchain;
    pchain.reserve(static_cast<size_t>(n_orders));
    hchain.reserve(static_cast<size_t>(n_orders));
    lob::Order* pprev = nullptr;
    lob::Order* hprev = nullptr;
    for (int i = 0; i < n_orders; ++i) {
        auto* p = pool.allocate();
        auto* h = heap.allocate();
        p->remaining_qty = 1;
        h->remaining_qty = 1;
        p->next = nullptr;
        h->next = nullptr;
        if (pprev) pprev->next = p;
        if (hprev) hprev->next = h;
        pprev = p;
        hprev = h;
        pchain.push_back(p);
        hchain.push_back(h);
    }

    volatile uint64_t sink = 0;
    auto walk = [&](lob::Order* head) {
        uint64_t s = 0;
        for (int w = 0; w < walks; ++w) {
            for (lob::Order* o = head; o; o = o->next) s += o->remaining_qty;
        }
        return s;
    };

    auto t0 = Clock::now();
    sink += walk(pchain.front());
    auto t1 = Clock::now();
    const double pool_ns =
        static_cast<double>(std::chrono::duration_cast<ns>(t1 - t0).count()) /
        static_cast<double>(walks);

    t0 = Clock::now();
    sink += walk(hchain.front());
    t1 = Clock::now();
    const double heap_ns =
        static_cast<double>(std::chrono::duration_cast<ns>(t1 - t0).count()) /
        static_cast<double>(walks);

    std::printf("order_walk n=%d walks=%d sizeof(Order)=%zu align=%zu  "
                "pool_list=%.1f ns/walk  heap_list=%.1f ns/walk  sink=%llu\n",
                n_orders, walks, sizeof(lob::Order), alignof(lob::Order), pool_ns,
                heap_ns, static_cast<unsigned long long>(sink));
}

// Naive std::list<Order> + new/delete vs slab intrusive list.
// Sequential list: nodes allocated back-to-back (best case for libc).
// Scattered list: 3 junk allocations between nodes so the chain is not
// adjacent — closer to a long-lived book that churns the heap.
static void bench_naive_list(int n_orders, int walks) {
    struct Node {
        uint64_t remaining_qty = 1;
        uint64_t pad[15]{}; // 128 bytes of payload, similar to Order
    };
    static_assert(sizeof(Node) == 128);

    std::list<Node> sequential;
    for (int i = 0; i < n_orders; ++i) sequential.push_back(Node{});

    std::list<Node> scattered;
    std::vector<void*> junk;
    junk.reserve(static_cast<size_t>(n_orders) * 3);
    for (int i = 0; i < n_orders; ++i) {
        for (int j = 0; j < 3; ++j) junk.push_back(::operator new(64));
        scattered.push_back(Node{});
    }

    volatile uint64_t sink = 0;
    auto walk = [&](std::list<Node>& lst) {
        uint64_t s = 0;
        for (int w = 0; w < walks; ++w) {
            for (auto& n : lst) s += n.remaining_qty;
        }
        return s;
    };

    auto t0 = Clock::now();
    sink += walk(sequential);
    auto t1 = Clock::now();
    const double seq_ns =
        static_cast<double>(std::chrono::duration_cast<ns>(t1 - t0).count()) /
        static_cast<double>(walks);

    t0 = Clock::now();
    sink += walk(scattered);
    t1 = Clock::now();
    const double scat_ns =
        static_cast<double>(std::chrono::duration_cast<ns>(t1 - t0).count()) /
        static_cast<double>(walks);

    for (void* p : junk) ::operator delete(p);

    std::printf("naive_std_list n=%d walks=%d sizeof(list_node_payload)=%zu  "
                "sequential_new=%.1f ns/walk  scattered_new=%.1f ns/walk  sink=%llu\n",
                n_orders, walks, sizeof(Node), seq_ns, scat_ns,
                static_cast<unsigned long long>(sink));
}

static void bench_alloc_churn(int n) {
    lob::OrderPool pool(static_cast<size_t>(n) + 8, lob::AllocMode::Pool);
    std::vector<lob::Order*> hold;
    hold.resize(static_cast<size_t>(n));

    auto t0 = Clock::now();
    for (int i = 0; i < n; ++i) hold[static_cast<size_t>(i)] = pool.allocate();
    for (int i = 0; i < n; ++i) pool.deallocate(hold[static_cast<size_t>(i)]);
    auto t1 = Clock::now();
    const double pool_ns =
        static_cast<double>(std::chrono::duration_cast<ns>(t1 - t0).count()) /
        static_cast<double>(n);

    lob::OrderPool heap(static_cast<size_t>(n) + 8, lob::AllocMode::Heap);
    t0 = Clock::now();
    for (int i = 0; i < n; ++i) hold[static_cast<size_t>(i)] = heap.allocate();
    for (int i = 0; i < n; ++i) heap.deallocate(hold[static_cast<size_t>(i)]);
    t1 = Clock::now();
    const double heap_ns =
        static_cast<double>(std::chrono::duration_cast<ns>(t1 - t0).count()) /
        static_cast<double>(n);

    std::list<lob::Order> lst;
    t0 = Clock::now();
    for (int i = 0; i < n; ++i) lst.emplace_back();
    lst.clear();
    t1 = Clock::now();
    const double list_ns =
        static_cast<double>(std::chrono::duration_cast<ns>(t1 - t0).count()) /
        static_cast<double>(n);

    std::printf("alloc_churn n=%d  pool_alloc+free=%.2f ns/op  "
                "heap_Order_new+delete=%.2f ns/op  std_list_emplace+clear=%.2f ns/op\n",
                n, pool_ns, heap_ns, list_ns);
}

static void run_pipeline(uint64_t events, bool with_md) {
    lob::OrderBook book(1 << 16);
    lob::MatchingEngine engine(book);
    lob::SimConfig sc;
    sc.target_depth_per_side = 100;
    sc.tick_span = 128;
    lob::OrderFlowSimulator sim(42, sc, book);

    md::Publisher* pub = nullptr;
    md::Publisher::Config pc;
    pc.group = "239.1.2.3";
    pc.port = 19011;
    pc.loopback = true;
    pc.pin_cpu = 1;
    pc.ttl = 1;
    md::Publisher pub_obj(pc);
    if (with_md) {
        if (!pub_obj.start()) {
            std::fprintf(stderr, "publisher start failed\n");
            std::exit(1);
        }
        pub = &pub_obj;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Warmup via simulator (book only) then rebuild live book through engine mix.
    for (int i = 0; i < 20000; ++i) sim.step();

    Acc acc_limit, acc_mkt, acc_cxl, acc_pub, acc_all;
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    std::vector<lob::order_id_t> live;
    live.reserve(512);

    const double z = sc.lambda_limit + sc.lambda_market + sc.lambda_cancel;

    auto t_run0 = Clock::now();
    for (uint64_t i = 0; i < events; ++i) {
        const uint64_t t_all0 = ns_now();
        double u = u01(rng) * z;
        if (u < sc.lambda_limit) {
            int span = std::max(1, sc.tick_span);
            int mag = 1 + static_cast<int>(u01(rng) * u01(rng) * static_cast<double>(span));
            bool buy = u01(rng) < 0.5;
            lob::price_t px = buy ? (sc.mid - mag) : (sc.mid + mag);
            const uint64_t t0 = ns_now();
            auto id = engine.submit_limit(buy ? lob::Side::Buy : lob::Side::Sell, px, sc.default_qty);
            const uint64_t t1 = ns_now();
            acc_limit.n++;
            acc_limit.ns_sum += (t1 - t0);
            if (id) {
                live.push_back(id);
                if (pub) {
                    const uint64_t p0 = ns_now();
                    pub->try_publish_add(id, buy ? lob::Side::Buy : lob::Side::Sell, px,
                                         sc.default_qty, p0);
                    acc_pub.n++;
                    acc_pub.ns_sum += (ns_now() - p0);
                }
            }
        } else if (u < sc.lambda_limit + sc.lambda_market) {
            bool buy = u01(rng) < 0.5;
            lob::qty_t q = 1 + static_cast<lob::qty_t>(rng() % 4);
            const uint64_t t0 = ns_now();
            auto fills = engine.submit_market(buy ? lob::Side::Buy : lob::Side::Sell, q);
            const uint64_t t1 = ns_now();
            acc_mkt.n++;
            acc_mkt.ns_sum += (t1 - t0);
            if (pub) {
                const uint64_t p0 = ns_now();
                for (const auto& f : fills) {
                    pub->try_publish_execute(f.resting_order_id, f.price, f.qty, p0);
                }
                pub->try_publish_snapshot(book, p0);
                acc_pub.n++;
                acc_pub.ns_sum += (ns_now() - p0);
            }
        } else {
            if (live.empty()) {
                auto id = engine.submit_limit(lob::Side::Buy, sc.mid - 1, 1);
                const uint64_t t1 = ns_now();
                acc_limit.n++;
                acc_limit.ns_sum += (t1 - t_all0);
                if (id) live.push_back(id);
            } else {
                size_t idx = static_cast<size_t>(rng() % live.size());
                auto id = live[idx];
                live[idx] = live.back();
                live.pop_back();
                const uint64_t t0 = ns_now();
                bool ok = book.cancel_order(id);
                const uint64_t t1 = ns_now();
                acc_cxl.n++;
                acc_cxl.ns_sum += (t1 - t0);
                if (ok && pub) {
                    const uint64_t p0 = ns_now();
                    pub->try_publish_cancel(id, p0);
                    acc_pub.n++;
                    acc_pub.ns_sum += (ns_now() - p0);
                }
            }
        }
        acc_all.n++;
        acc_all.ns_sum += (ns_now() - t_all0);
    }
    auto t_run1 = Clock::now();
    const double wall_s =
        std::chrono::duration<double>(t_run1 - t_run0).count();

    std::printf("pipeline events=%llu md=%d wall=%.4f s  events/s=%.0f  "
                "best_bid=%lld best_ask=%lld live~%zu\n",
                static_cast<unsigned long long>(events), with_md ? 1 : 0, wall_s,
                static_cast<double>(events) / wall_s,
                book.best_bid() ? static_cast<long long>(*book.best_bid()) : -1,
                book.best_ask() ? static_cast<long long>(*book.best_ask()) : -1,
                live.size());
    report("submit_limit", acc_limit);
    report("submit_market", acc_mkt);
    report("cancel_order", acc_cxl);
    report("publisher_enqueue", acc_pub);
    report("event_total_matching_thread", acc_all);

    if (pub) {
        std::printf("  publisher packets=%llu msgs=%llu dropped=%llu send_fail=%llu\n",
                    static_cast<unsigned long long>(pub->stats().packets.load()),
                    static_cast<unsigned long long>(pub->stats().messages.load()),
                    static_cast<unsigned long long>(pub->stats().dropped.load()),
                    static_cast<unsigned long long>(pub->stats().send_fail.load()));
        pub_obj.stop();
    }
}

int main(int argc, char** argv) {
    uint64_t events = 400000;
    bool with_md = true;
    bool hold = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--events=", 9) == 0) events = std::strtoull(argv[i] + 9, nullptr, 10);
        else if (std::strcmp(argv[i], "--no-md") == 0) with_md = false;
        else if (std::strcmp(argv[i], "--hold") == 0) hold = true;
    }

    std::printf("host=Apple Silicon  sizeof(Order)=%zu  align=%zu  "
                "sysctl_cacheline (see docs)\n",
                sizeof(lob::Order), alignof(lob::Order));

    for (int d : {8, 32, 64, 128, 256, 512}) {
        bench_map_vs_vector(d, 400000);
    }
    bench_order_walk(64, 20000);
    bench_order_walk(256, 8000);
    bench_order_walk(2048, 2000);
    bench_order_walk(8192, 400);
    bench_naive_list(64, 20000);
    bench_naive_list(256, 8000);
    bench_naive_list(2048, 2000);
    bench_naive_list(8192, 400);
    bench_alloc_churn(200000);

    run_pipeline(events, with_md);

    if (hold) {
        std::puts("holding 8s for sample(1)...");
        std::fflush(stdout);
        // Keep matching so sample sees the hot path, not idle.
        run_pipeline(events, with_md);
    }
    return 0;
}
