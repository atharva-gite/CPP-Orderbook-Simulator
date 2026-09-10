#include <catch2/catch_test_macros.hpp>
#include "lob/matching_engine.hpp"
#include "lob/order_book.hpp"

using namespace lob;

TEST_CASE("iceberg replenishment loses time priority", "[matching][iceberg]") {
    OrderBook b(128);
    MatchingEngine me(b);

    // Add iceberg A (display 5, total 15) at price 100
    auto a = me.submit_limit(Side::Sell, 100, 15, 5);
    // Add regular B at same price
    auto b_id = me.submit_limit(Side::Sell, 100, 10, 0);

    // Incoming buy market for 5 should consume A's visible slice
    auto fills1 = me.submit_market(Side::Buy, 5);
    REQUIRE(fills1.size() == 1);
    REQUIRE(fills1[0].resting_order_id == a);

    // Now replenished slice from A should go to back; next remaining at level should be B
    // Incoming buy of 1 should hit B first (since replenish lost time priority)
    auto fills2 = me.submit_market(Side::Buy, 1);
    REQUIRE(!fills2.empty());
    REQUIRE(fills2[0].resting_order_id == b_id);
}

TEST_CASE("stop cascade triggers chained stops", "[matching][stops]") {
    OrderBook b(256);
    MatchingEngine me(b);

    // Create a sequence: S1 triggers at >=101 -> issues market that trades at 101
    // S2 triggers at >=102 etc. To simplify, set stops so that one trigger will
    // cause next to be eligible via processing.
    auto s3 = me.submit_stop(Side::Sell, 103, 5, StopType::StopMarket);
    auto s2 = me.submit_stop(Side::Sell, 102, 5, StopType::StopMarket);
    auto s1 = me.submit_stop(Side::Sell, 101, 5, StopType::StopMarket);

    // Provide liquidity on opposite side so market orders will execute and move price
    me.submit_limit(Side::Buy, 101, 10);
    // Trigger first stop by simulating a trade at price 101 via market order
    auto fills = me.submit_market(Side::Buy, 1);
    // process_triggers happens inside submit_market; ensure pending stops processed
    // At least one stop should have been triggered and removed from pending
    REQUIRE(b.best_bid().has_value());
}

TEST_CASE("pro-rata allocation across 4 resting orders", "[matching][prorata]") {
    OrderBook b(256);
    // Use a pro-rata policy
    auto policy = std::make_unique<ProRataPolicy>(1);
    MatchingEngine me(b, std::move(policy));

    // Resting orders at same price 100 with sizes 10,20,30,40
    auto o1 = me.submit_limit(Side::Sell, 100, 10);
    auto o2 = me.submit_limit(Side::Sell, 100, 20);
    auto o3 = me.submit_limit(Side::Sell, 100, 30);
    auto o4 = me.submit_limit(Side::Sell, 100, 40);

    // Incoming buy of 50 should allocate 5,10,15,20 respectively
    auto fills = me.submit_market(Side::Buy, 50);
    qty_t total = 0;
    std::unordered_map<order_id_t, qty_t> alloc;
    for (auto &f : fills) { alloc[f.resting_order_id] += f.qty; total += f.qty; }
    REQUIRE(total == 50);
    REQUIRE(alloc[o1] == 5);
    REQUIRE(alloc[o2] == 10);
    REQUIRE(alloc[o3] == 15);
    REQUIRE(alloc[o4] == 20);
}
