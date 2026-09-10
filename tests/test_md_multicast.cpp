#include <catch2/catch_test_macros.hpp>
#include "lob/order_book.hpp"
#include "md_publisher.hpp"
#include "md_receiver.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>
#include <vector>

static std::atomic<int> g_alloc_count{0};

void* operator new(std::size_t sz) {
    ++g_alloc_count;
    if (void* p = std::malloc(sz)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

void* operator new[](std::size_t sz) {
    ++g_alloc_count;
    if (void* p = std::malloc(sz)) return p;
    throw std::bad_alloc();
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

void* operator new(std::size_t sz, std::align_val_t align) {
    ++g_alloc_count;
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(align), sz) == 0) return p;
    throw std::bad_alloc();
}
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }

static double pct_of(const uint64_t* a, uint32_t n, double q) {
    if (n == 0) return 0;
    std::vector<uint64_t> v(a, a + n);
    std::sort(v.begin(), v.end());
    const double idx = q * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return static_cast<double>(v[lo]) * (1.0 - frac) + static_cast<double>(v[hi]) * frac;
}

static void print_hist(const char* name, const uint64_t* a, uint32_t n) {
    static constexpr uint64_t edges[] = {1000, 5000, 10000, 25000, 50000, 100000, 250000, 1000000};
    uint32_t bins[9]{};
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t b = 0;
        while (b < 8 && a[i] >= edges[b]) ++b;
        ++bins[b];
    }
    std::cout << "  " << name << " hist ns: ";
    uint64_t prev = 0;
    for (int i = 0; i < 8; ++i) {
        std::cout << "[" << prev << "," << edges[i] << ")=" << bins[i] << " ";
        prev = edges[i];
    }
    std::cout << "[>=" << edges[7] << "]=" << bins[8] << "\n";
}

TEST_CASE("ITCH wire format pack/unpack, packed sizes, endianness", "[net][protocol]") {
    REQUIRE(sizeof(md::PktHdr) == 24);
    REQUIRE(sizeof(md::AddBody) == 33);
    REQUIRE(sizeof(md::CancelBody) == 16);
    REQUIRE(sizeof(md::ExecuteBody) == 32);
    REQUIRE(sizeof(md::SnapshotBody) == 41);

    uint8_t buf[64];

    md::Event add;
    add.type = md::MsgType::Add;
    add.side = lob::Side::Sell;
    add.order_id = 0x0102030405060708ull;
    add.price = 12345;
    add.qty = 99;
    add.event_ns = 0x1122334455667788ull;
    REQUIRE(md::pack_msg(buf, add) == 1 + sizeof(md::AddBody));
    md::Decoded d;
    REQUIRE(md::unpack_msg(buf, sizeof(buf), d) == 1 + sizeof(md::AddBody));
    REQUIRE(d.type == md::MsgType::Add);
    REQUIRE(d.side == lob::Side::Sell);
    REQUIRE(d.order_id == add.order_id);
    REQUIRE(d.price == add.price);
    REQUIRE(d.qty == add.qty);
    REQUIRE(d.event_ns == add.event_ns);

    md::Event can;
    can.type = md::MsgType::Cancel;
    can.order_id = 7;
    can.event_ns = 8;
    REQUIRE(md::pack_msg(buf, can) == 1 + sizeof(md::CancelBody));
    REQUIRE(md::unpack_msg(buf, sizeof(buf), d) == 1 + sizeof(md::CancelBody));
    REQUIRE(d.order_id == 7);

    md::Event ex;
    ex.type = md::MsgType::Execute;
    ex.order_id = 3;
    ex.price = 50;
    ex.qty = 2;
    ex.event_ns = 9;
    REQUIRE(md::pack_msg(buf, ex) == 1 + sizeof(md::ExecuteBody));
    REQUIRE(md::unpack_msg(buf, sizeof(buf), d) == 1 + sizeof(md::ExecuteBody));
    REQUIRE(d.price == 50);
    REQUIRE(d.qty == 2);

    md::Event sn;
    sn.type = md::MsgType::Snapshot;
    sn.snap_flags = 3;
    sn.bid = 100;
    sn.bid_qty = 10;
    sn.ask = 101;
    sn.ask_qty = 11;
    sn.event_ns = 1;
    REQUIRE(md::pack_msg(buf, sn) == 1 + sizeof(md::SnapshotBody));
    REQUIRE(md::unpack_msg(buf, sizeof(buf), d) == 1 + sizeof(md::SnapshotBody));
    REQUIRE(d.snap_flags == 3);
    REQUIRE(d.bid == 100);
    REQUIRE(d.ask == 101);
}

TEST_CASE("UDP multicast ITCH round-trip, no heap on publish hot path", "[net][md]") {
    md::Receiver::Config rc;
    rc.group = "239.1.2.3";
    rc.port = 19001;
    rc.pin_cpu = 2;
    md::Publisher::Config pc;
    pc.group = rc.group;
    pc.port = rc.port;
    pc.loopback = true;
    pc.pin_cpu = 1; // Linux: hard pin away from matching/main. macOS: affinity hint only.
    pc.ttl = 1;

    md::Receiver rx(rc);
    REQUIRE(rx.start());

    std::thread rx_th([&] { rx.run_loop(); });

    md::Publisher pub(pc);
    REQUIRE(pub.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    lob::OrderBook book(256);
    const auto id_buy = book.add_limit_order(lob::Side::Buy, 100, 10);
    const auto id_sell = book.add_limit_order(lob::Side::Sell, 101, 10);
    REQUIRE(id_buy != 0);
    REQUIRE(id_sell != 0);

    constexpr int kN = 400;
    g_alloc_count.store(0, std::memory_order_relaxed);

    int failed = 0;
    for (int i = 0; i < kN; ++i) {
        const uint64_t t = md::now_ns();
        const int kind = i % 4;
        bool ok = false;
        if (kind == 0) {
            ok = pub.try_publish_add(static_cast<lob::order_id_t>(1000 + i), lob::Side::Buy,
                                     100 + (i % 3), 1, t);
        } else if (kind == 1) {
            ok = pub.try_publish_cancel(static_cast<lob::order_id_t>(1000 + i), t);
        } else if (kind == 2) {
            ok = pub.try_publish_execute(id_sell, 101, 1, t);
        } else {
            ok = pub.try_publish_snapshot(book, t);
        }
        if (!ok) ++failed;
    }

    const int hot_allocs = g_alloc_count.load(std::memory_order_relaxed);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (rx.stats().messages.load(std::memory_order_relaxed) < static_cast<uint64_t>(kN) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    rx.stop();
    rx_th.join();
    rx.close_fd();
    pub.stop();

    REQUIRE(failed == 0);
    REQUIRE(hot_allocs == 0);
    REQUIRE(rx.stats().messages.load() == static_cast<uint64_t>(kN));
    REQUIRE(pub.stats().send_fail.load() == 0);
    REQUIRE(rx.stats().bad.load() == 0);

    const auto& last = rx.stats().last;
    REQUIRE(last.type == md::MsgType::Snapshot);
    REQUIRE((last.snap_flags & 1u) != 0);
    REQUIRE((last.snap_flags & 2u) != 0);
    REQUIRE(last.bid == 100);
    REQUIRE(last.ask == 101);
    REQUIRE(last.bid_qty == 10);
    REQUIRE(last.ask_qty == 10);

    const uint32_t n_rtt = std::min(rx.stats().n_rtt.load(), static_cast<uint32_t>(md::Receiver::kHist));
    const uint32_t n_pub = std::min(pub.stats().n_pub.load(), static_cast<uint32_t>(md::Publisher::kHist));
    const uint32_t n_ser = std::min(pub.stats().n_ser.load(), static_cast<uint32_t>(md::Publisher::kHist));
    REQUIRE(n_rtt >= static_cast<uint32_t>(kN));
    REQUIRE(n_pub >= static_cast<uint32_t>(kN));
    REQUIRE(n_ser > 0);

    const double rtt_p50 = pct_of(rx.stats().rtt_ns, n_rtt, 0.50);
    const double rtt_p99 = pct_of(rx.stats().rtt_ns, n_rtt, 0.99);
    const double pub_p50 = pct_of(pub.stats().pub_ns, n_pub, 0.50);
    const double pub_p99 = pct_of(pub.stats().pub_ns, n_pub, 0.99);
    const double ser_p50 = pct_of(pub.stats().ser_ns, n_ser, 0.50);
    const double ser_p99 = pct_of(pub.stats().ser_ns, n_ser, 0.99);

    std::cout << "\nMD multicast loopback (N=" << kN << " msgs)\n"
              << "  hot-path heap allocs: " << hot_allocs << "\n"
              << "  publish event->sendto  p50=" << pub_p50 << " ns  p99=" << pub_p99 << " ns\n"
              << "  serialize/msg          p50=" << ser_p50 << " ns  p99=" << ser_p99 << " ns\n"
              << "  round-trip event->recv p50=" << rtt_p50 << " ns  p99=" << rtt_p99 << " ns\n"
              << "  packets=" << pub.stats().packets.load() << "  (batched)\n";
    print_hist("publish", pub.stats().pub_ns, n_pub);
    print_hist("serialize", pub.stats().ser_ns, n_ser);
    print_hist("rtt", rx.stats().rtt_ns, n_rtt);

    REQUIRE(pub_p50 > 0);
    REQUIRE(rtt_p50 > 0);
}
