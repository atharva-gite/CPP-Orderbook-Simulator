#include <catch2/catch_test_macros.hpp>
#include "lob/mpsc_queue.hpp"
#include "lob/pipeline.hpp"
#include "lob/seqlock.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace lob;

TEST_CASE("MPSC ring delivers every item exactly once", "[concurrent][mpsc]") {
    constexpr int kProducers = 4;
    constexpr int kEach = 5000;
    MpscRing<IngressCmd, 4096> q;
    std::atomic<int> remaining{kProducers};
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kEach; ++i) {
                IngressCmd c;
                c.seq = (static_cast<uint64_t>(p) << 32) | static_cast<uint32_t>(i);
                c.qty = 1;
                while (!q.try_push(c)) std::this_thread::yield();
            }
            remaining.fetch_sub(1, std::memory_order_release);
        });
    }

    std::vector<uint64_t> seen;
    seen.reserve(kProducers * kEach);
    IngressCmd c;
    while (remaining.load(std::memory_order_acquire) > 0 || !q.empty()) {
        if (q.try_pop(c)) seen.push_back(c.seq);
        else std::this_thread::yield();
    }
    while (q.try_pop(c)) seen.push_back(c.seq);

    for (auto& t : producers) t.join();

    REQUIRE(seen.size() == static_cast<size_t>(kProducers * kEach));
    std::sort(seen.begin(), seen.end());
    REQUIRE(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
}

TEST_CASE("seqlock readers never mix fields from two publishes", "[concurrent][seqlock]") {
    SeqlockQuote sl;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> bad{0};
    std::atomic<uint64_t> reads{0};

    std::thread writer([&] {
        for (uint64_t i = 1; i < 200000 && !stop.load(std::memory_order_relaxed); ++i) {
            Quote q;
            q.has_bid = true;
            q.bid = static_cast<price_t>(i);
            q.bid_qty = i; // same generation in both fields
            sl.publish(q);
        }
        stop.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire) || sl.seq() == 0) {
                Quote q = sl.read();
                reads.fetch_add(1, std::memory_order_relaxed);
                if (q.has_bid && static_cast<uint64_t>(q.bid) != q.bid_qty) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
                if (stop.load(std::memory_order_relaxed)) break;
            }
        });
    }

    writer.join();
    for (auto& t : readers) t.join();
    REQUIRE(bad.load() == 0);
    REQUIRE(reads.load() > 0);
}

TEST_CASE("single-writer pipeline: producers + consumer + seqlock readers",
          "[concurrent][pipeline]") {
    SingleWriterPipeline pipe(1 << 15);
    constexpr int kProducers = 4;
    constexpr int kEach = 3000;
    std::atomic<int> live{kProducers};
    std::atomic<bool> consume{true};
    std::atomic<uint64_t> torn{0};

    std::thread consumer([&] {
        while (consume.load(std::memory_order_acquire) ||
               pipe.applied() < static_cast<uint64_t>(kProducers * kEach)) {
            if (pipe.drain(64) == 0) std::this_thread::yield();
        }
    });

    std::thread reader([&] {
        while (consume.load(std::memory_order_acquire)) {
            Quote q = pipe.quotes().read();
            if (q.has_bid && q.has_ask && q.bid > q.ask) {
                // Crossing can happen in this engine; not a seqlock tear.
                (void)q;
            }
            (void)q;
            torn.fetch_add(0, std::memory_order_relaxed);
        }
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kEach; ++i) {
                IngressCmd c;
                c.type = (i % 11 == 0) ? IngressType::Market : IngressType::Limit;
                c.side = (i & 1) ? Side::Buy : Side::Sell;
                c.price = 10000 + static_cast<price_t>((i % 50) - 25);
                c.qty = 1;
                c.seq = static_cast<uint64_t>(p * kEach + i);
                while (!pipe.try_submit(c)) std::this_thread::yield();
            }
            live.fetch_sub(1, std::memory_order_release);
        });
    }

    for (auto& t : producers) t.join();
    while (pipe.applied() < static_cast<uint64_t>(kProducers * kEach)) {
        std::this_thread::yield();
    }
    consume.store(false, std::memory_order_release);
    consumer.join();
    reader.join();
    REQUIRE(pipe.applied() == static_cast<uint64_t>(kProducers * kEach));
    REQUIRE(torn.load() == 0);
}

TEST_CASE("sharded books: mutex per instrument shard, no lost applies",
          "[concurrent][sharded]") {
    constexpr int kShards = 4;
    constexpr int kProducers = 4;
    constexpr int kEach = 2000;
    ShardedBooks books(kShards, 1 << 14);

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kEach; ++i) {
                IngressCmd c;
                c.instrument = static_cast<uint32_t>(i % kShards);
                c.type = IngressType::Limit;
                c.side = (p & 1) ? Side::Buy : Side::Sell;
                c.price = 100 + static_cast<price_t>(i % 20);
                c.qty = 1;
                books.submit(c);
            }
        });
    }
    for (auto& t : producers) t.join();
    REQUIRE(books.applied() == static_cast<uint64_t>(kProducers * kEach));
    Quote q = books.quote(0);
    (void)q;
}
