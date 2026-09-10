#include <catch2/catch_test_macros.hpp>
#include "lob/order_book.hpp"

using namespace lob;

TEST_CASE("insert then cancel restores empty book", "[orderbook]") {
    OrderBook b(128);
    auto id = b.add_limit_order(Side::Buy, 100, 10);
    REQUIRE(id != 0);
    REQUIRE(b.best_bid().has_value());
    REQUIRE(b.cancel_order(id));
    REQUIRE(!b.best_bid().has_value());
}

TEST_CASE("FIFO within price level under partial fills", "[orderbook]") {
    OrderBook b(128);
    auto id1 = b.add_limit_order(Side::Sell, 200, 10);
    auto id2 = b.add_limit_order(Side::Sell, 200, 20);

    auto fills = b.market_order(Side::Buy, 15);
    REQUIRE(fills.size() == 2);
    REQUIRE(fills[0].resting_order_id == id1);
    REQUIRE(fills[0].qty == 10);
    REQUIRE(fills[1].resting_order_id == id2);
    REQUIRE(fills[1].qty == 5);
}

TEST_CASE("market order walks multiple levels and computes average price", "[orderbook]") {
    OrderBook b(256);
    b.add_limit_order(Side::Sell, 101, 10);
    b.add_limit_order(Side::Sell, 102, 20);
    b.add_limit_order(Side::Sell, 103, 30);

    auto pre_mid = b.midprice();
    auto fills = b.market_order(Side::Buy, 50);
    qty_t total = 0; long long weighted = 0;
    for (auto &f : fills) { total += f.qty; weighted += static_cast<long long>(f.qty) * f.price; }
    REQUIRE(total == 50);
    double avg = static_cast<double>(weighted) / static_cast<double>(total);
    // avg should be between best ask and worst filled price
    REQUIRE(avg >= 101.0);
    REQUIRE(avg <= 103.0);
}

TEST_CASE("cancel/modify nonexistent id fails cleanly", "[orderbook]") {
    OrderBook b(16);
    REQUIRE(!b.cancel_order(999999));
    REQUIRE(!b.modify_order(999999, 100, 10));
}

TEST_CASE("empty-book edge cases", "[orderbook]") {
    OrderBook b(4);
    auto fills = b.market_order(Side::Buy, 10);
    REQUIRE(fills.empty());
    REQUIRE(!b.best_bid().has_value());
    REQUIRE(!b.best_ask().has_value());
}

TEST_CASE("manual trace snapshot", "[orderbook][trace]") {
    OrderBook b(64);
    b.add_limit_order(Side::Buy, 100, 10);
    b.add_limit_order(Side::Buy, 99, 5);
    b.add_limit_order(Side::Sell, 101, 7);
    b.add_limit_order(Side::Sell, 102, 8);
    auto snap1 = b.snapshot(10);
    REQUIRE(!snap1.empty());
    // perform some ops and print
    auto id = b.add_limit_order(Side::Buy, 100, 3);
    b.cancel_order(id);
    auto snap2 = b.snapshot(10);
    REQUIRE(!snap2.empty());
}
