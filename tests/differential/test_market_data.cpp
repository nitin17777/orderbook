#include <catch2/catch_test_macros.hpp>

#include "test_command.hpp"
#include "test_generator.hpp"
#include "test_market_data.hpp"

#include "orderbook/book.hpp"
#include "orderbook/fast_book.hpp"
#include "orderbook/market_data.hpp"
#include "orderbook/engine.hpp"

using namespace orderbook;
using namespace orderbook::test;

// ── 1. Per-Level Aggregate Correctness ────────────────────────────────────────
//
// Checks that bid_level_qty() / ask_level_qty() on both OrderBook and
// FastOrderBook match a direct scan of resting open quantities after every
// single command in the sequence.

TEST_CASE("L2 Aggregates: resting order increments level qty", "[l2][aggregate]") {
    OrderBook book;
    Order o{};
    o.id = 1; o.side = Side::Buy; o.type = OrderType::Limit;
    o.status = OrderStatus::Accepted; o.price = 100; o.quantity = 15; o.filled = 0;
    book.add(o);

    REQUIRE(book.bid_level_qty(100) == 15);
    REQUIRE(book.ask_level_qty(100) == 0);
}

TEST_CASE("L2 Aggregates: fill reduces level qty", "[l2][aggregate]") {
    OrderBook book;
    Order maker{};
    maker.id = 1; maker.side = Side::Sell; maker.type = OrderType::Limit;
    maker.status = OrderStatus::Accepted; maker.price = 100; maker.quantity = 20; maker.filled = 0;
    book.add(maker);
    REQUIRE(book.ask_level_qty(100) == 20);

    Order taker{};
    taker.id = 2; taker.side = Side::Buy; taker.type = OrderType::Limit;
    taker.status = OrderStatus::Accepted; taker.price = 100; taker.quantity = 12; taker.filled = 0;
    book.add(taker);
    REQUIRE(book.ask_level_qty(100) == 8);  // 20 - 12
}

TEST_CASE("L2 Aggregates: cancel reduces level qty immediately (lazy-cancel policy)", "[l2][aggregate]") {
    OrderBook book;
    Order o1{};
    o1.id = 1; o1.side = Side::Buy; o1.type = OrderType::Limit;
    o1.status = OrderStatus::Accepted; o1.price = 99; o1.quantity = 10; o1.filled = 0;
    book.add(o1);

    Order o2{};
    o2.id = 2; o2.side = Side::Buy; o2.type = OrderType::Limit;
    o2.status = OrderStatus::Accepted; o2.price = 99; o2.quantity = 5; o2.filled = 0;
    book.add(o2);

    REQUIRE(book.bid_level_qty(99) == 15);

    book.cancel(1);
    REQUIRE(book.bid_level_qty(99) == 5);   // reduced immediately, not on tombstone skip

    book.cancel(2);
    REQUIRE(book.bid_level_qty(99) == 0);   // level removed
}

TEST_CASE("L2 Aggregates: FastOrderBook lazy-cancel policy", "[l2][aggregate][fast]") {
    FastOrderBook book;
    Order o1{};
    o1.id = 1; o1.side = Side::Sell; o1.type = OrderType::Limit;
    o1.status = OrderStatus::Accepted; o1.price = 105; o1.quantity = 30; o1.filled = 0;
    book.add(o1);

    Order o2{};
    o2.id = 2; o2.side = Side::Sell; o2.type = OrderType::Limit;
    o2.status = OrderStatus::Accepted; o2.price = 105; o2.quantity = 20; o2.filled = 0;
    book.add(o2);

    REQUIRE(book.ask_level_qty(105) == 50);

    // Cancel o1 — tombstone left in deque, but aggregate must drop immediately.
    book.cancel(1);
    REQUIRE(book.ask_level_qty(105) == 20);

    // Now a taker arrives — it walks past the tombstone, matches o2.
    Order taker{};
    taker.id = 3; taker.side = Side::Buy; taker.type = OrderType::Limit;
    taker.status = OrderStatus::Accepted; taker.price = 110; taker.quantity = 10; taker.filled = 0;
    book.add(taker);
    REQUIRE(book.ask_level_qty(105) == 10);   // 20 - 10 filled
}

TEST_CASE("L2 Aggregates: multi-level sweep correctness", "[l2][aggregate]") {
    OrderBook book;
    for (int p = 100; p <= 104; ++p) {
        Order o{};
        o.id = (OrderId)p; o.side = Side::Sell; o.type = OrderType::Limit;
        o.status = OrderStatus::Accepted; o.price = p; o.quantity = 10; o.filled = 0;
        book.add(o);
    }
    // Taker sweeps 100, 101, 102 entirely, partial on 103
    Order taker{};
    taker.id = 999; taker.side = Side::Buy; taker.type = OrderType::Limit;
    taker.status = OrderStatus::Accepted; taker.price = 103; taker.quantity = 35; taker.filled = 0;
    book.add(taker);

    REQUIRE(book.ask_level_qty(100) == 0);
    REQUIRE(book.ask_level_qty(101) == 0);
    REQUIRE(book.ask_level_qty(102) == 0);
    REQUIRE(book.ask_level_qty(103) == 5);   // 10 - 5 filled
    REQUIRE(book.ask_level_qty(104) == 10);  // untouched
}

TEST_CASE("L2 Aggregates: randomized aggregate correctness (OrderBook + FastOrderBook)",
          "[l2][aggregate][fuzz]") {
    constexpr size_t NUM_SEEDS = 20;
    constexpr size_t OPS_PER_SEED = 300;

    for (uint64_t seed = 1; seed <= NUM_SEEDS; ++seed) {
        CommandGenerator gen(seed * 31337, 95, 105);
        auto sequence = gen.generate(OPS_PER_SEED);

        auto result = run_l2_aggregate_check(sequence);
        if (!result.passed) {
            UNSCOPED_INFO("Seed: " << (seed * 31337));
            UNSCOPED_INFO("Failing step: " << result.failing_step);
            UNSCOPED_INFO("Failing cmd: " << result.failing_command);
            UNSCOPED_INFO("Error: " << result.error);
        }
        REQUIRE(result.passed);
    }
}

// ── 2. Snapshot + Deltas Protocol ────────────────────────────────────────────

TEST_CASE("L2 Publisher: initial snapshot is empty", "[l2][publisher]") {
    L2Publisher pub;
    auto snap = pub.snapshot();
    REQUIRE(snap.seq == 0);
    REQUIRE(snap.bids.empty());
    REQUIRE(snap.asks.empty());
}

TEST_CASE("L2 Publisher: apply_delta gap detection", "[l2][publisher]") {
    L2Snapshot snap;
    snap.seq = 5;

    // Good delta: prev_seq == 5
    L2Delta good{6, 5, Side::Buy, 100, 10};
    REQUIRE(apply_delta(snap, good));
    REQUIRE(snap.seq == 6);

    // Gap: prev_seq == 4, but we're at seq 6
    L2Delta gap{7, 4, Side::Buy, 100, 20};
    REQUIRE_FALSE(apply_delta(snap, gap));
    REQUIRE(snap.seq == 6);  // unchanged on gap
}

TEST_CASE("L2 Publisher: apply_delta level insertion and removal", "[l2][publisher]") {
    L2Snapshot snap;
    snap.seq = 0;

    // Insert bid @100 qty=10
    L2Delta d1{1, 0, Side::Buy, 100, 10};
    REQUIRE(apply_delta(snap, d1));
    REQUIRE(snap.bids.size() == 1);
    REQUIRE(snap.bids[0].price == 100);
    REQUIRE(snap.bids[0].qty == 10);

    // Insert ask @105 qty=5
    L2Delta d2{2, 1, Side::Sell, 105, 5};
    REQUIRE(apply_delta(snap, d2));
    REQUIRE(snap.asks.size() == 1);

    // Update bid @100 qty=15
    L2Delta d3{3, 2, Side::Buy, 100, 15};
    REQUIRE(apply_delta(snap, d3));
    REQUIRE(snap.bids[0].qty == 15);

    // Remove bid @100 (new_qty=0)
    L2Delta d4{4, 3, Side::Buy, 100, 0};
    REQUIRE(apply_delta(snap, d4));
    REQUIRE(snap.bids.empty());
}

TEST_CASE("L2 Publisher: bids maintain descending price order", "[l2][publisher]") {
    L2Snapshot snap;
    snap.seq = 0;

    // Insert in non-sorted order
    L2Delta d1{1, 0, Side::Buy, 100, 10};
    L2Delta d2{2, 1, Side::Buy, 102, 5};
    L2Delta d3{3, 2, Side::Buy, 101, 8};

    REQUIRE(apply_delta(snap, d1));
    REQUIRE(apply_delta(snap, d2));
    REQUIRE(apply_delta(snap, d3));

    REQUIRE(snap.bids.size() == 3);
    REQUIRE(snap.bids[0].price == 102);
    REQUIRE(snap.bids[1].price == 101);
    REQUIRE(snap.bids[2].price == 100);
}

TEST_CASE("L2 Publisher: asks maintain ascending price order", "[l2][publisher]") {
    L2Snapshot snap;
    snap.seq = 0;

    L2Delta d1{1, 0, Side::Sell, 105, 10};
    L2Delta d2{2, 1, Side::Sell, 103, 5};
    L2Delta d3{3, 2, Side::Sell, 104, 8};

    REQUIRE(apply_delta(snap, d1));
    REQUIRE(apply_delta(snap, d2));
    REQUIRE(apply_delta(snap, d3));

    REQUIRE(snap.asks.size() == 3);
    REQUIRE(snap.asks[0].price == 103);
    REQUIRE(snap.asks[1].price == 104);
    REQUIRE(snap.asks[2].price == 105);
}

// ── 3. Reconstruction Equivalence Harness ────────────────────────────────────
//
// The core completion criterion: depth reconstructed from snapshot + deltas
// must equal a fresh scan of the book after arbitrary random sequences.

TEST_CASE("L2 Reconstruction: Scenario 1 - single resting limit order", "[l2][reconstruct]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Buy,  100, 10, 1),
        TestCommand::limit(2, Side::Sell, 105, 10, 2)
    };
    auto result = run_l2_reconstruction_check(cmds);
    if (!result.passed) {
        UNSCOPED_INFO("Error: " << result.error);
        UNSCOPED_INFO(l2_diff(result.reconstructed, result.ground_truth));
    }
    REQUIRE(result.passed);
}

TEST_CASE("L2 Reconstruction: Scenario 2 - exact match removes levels", "[l2][reconstruct]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 25, 1),
        TestCommand::limit(2, Side::Buy,  100, 25, 2)  // fully fills level
    };
    auto result = run_l2_reconstruction_check(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("L2 Reconstruction: Scenario 3 - partial fill leaves residual", "[l2][reconstruct]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 20, 1),
        TestCommand::limit(2, Side::Buy,  100, 15, 2)  // 5 of maker 1 remains
    };
    auto result = run_l2_reconstruction_check(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("L2 Reconstruction: Scenario 4 - multi-level sweep", "[l2][reconstruct]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 10, 1),
        TestCommand::limit(2, Side::Sell, 101, 10, 2),
        TestCommand::limit(3, Side::Sell, 102, 10, 3),
        TestCommand::limit(4, Side::Buy,  102, 25, 4)  // sweeps 100, 101, partial 102
    };
    auto result = run_l2_reconstruction_check(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("L2 Reconstruction: Scenario 5 - cancel adjusts depth", "[l2][reconstruct]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Buy, 100, 10, 1),
        TestCommand::limit(2, Side::Buy, 100, 20, 2),
        TestCommand::limit(3, Side::Buy,  99, 15, 3),
        TestCommand::cancel(1),
        TestCommand::limit(4, Side::Sell, 100, 10, 4)
    };
    auto result = run_l2_reconstruction_check(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("L2 Reconstruction: Scenario 6 - same-price FIFO and cancel", "[l2][reconstruct]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 10, 1),
        TestCommand::limit(2, Side::Sell, 100, 10, 2),
        TestCommand::limit(3, Side::Sell, 100, 10, 3),
        TestCommand::cancel(2),
        TestCommand::limit(4, Side::Buy,  100, 15, 4)  // fills 1 and part of 3
    };
    auto result = run_l2_reconstruction_check(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("L2 Reconstruction: Scenario 7 - deep order book multiple levels", "[l2][reconstruct]") {
    std::vector<TestCommand> cmds;
    // Build a 5-level bid book
    for (int p = 96; p <= 100; ++p)
        cmds.push_back(TestCommand::limit((OrderId)p, Side::Buy, p, 10, (Timestamp)p));
    // Build a 5-level ask book
    for (int p = 101; p <= 105; ++p)
        cmds.push_back(TestCommand::limit((OrderId)p, Side::Sell, p, 10, (Timestamp)p));
    // Sweep 2 ask levels
    cmds.push_back(TestCommand::limit(200, Side::Buy, 102, 15, 200));
    // Cancel a bid level
    cmds.push_back(TestCommand::cancel(98));

    auto result = run_l2_reconstruction_check(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("L2 Reconstruction: randomized equivalence (snapshot+deltas == book scan)",
          "[l2][reconstruct][fuzz]") {
    constexpr size_t NUM_SEEDS = 25;
    constexpr size_t OPS_PER_SEED = 200;

    for (uint64_t seed = 1; seed <= NUM_SEEDS; ++seed) {
        CommandGenerator gen(seed * 2053, 98, 102);
        auto sequence = gen.generate(OPS_PER_SEED);

        auto result = run_l2_reconstruction_check(sequence);
        if (!result.passed) {
            UNSCOPED_INFO("Seed: " << (seed * 2053));
            UNSCOPED_INFO("Failing step: " << result.failing_step);
            UNSCOPED_INFO("Failing cmd: " << result.failing_command);
            UNSCOPED_INFO("Error: " << result.error);
        }
        REQUIRE(result.passed);
    }
}

// ── 4. Snapshot Consistency ───────────────────────────────────────────────────
// A snapshot taken at any point in time must have the same depth as the book.

TEST_CASE("L2 Publisher: snapshot consistency after operations", "[l2][publisher][snapshot]") {
    Engine engine("tmp_snapshot_test.log");
    L2Publisher pub;

    auto add_order = [&](OrderId id, Side side, Price price, Quantity qty) {
        Order o{};
        o.id = id; o.side = side; o.type = OrderType::Limit;
        o.status = OrderStatus::Accepted; o.price = price; o.quantity = qty;
        VectorSink sink;
        engine.add(o, sink);
        for (const auto& ev : sink.events) {
            if (ev.tag() == EventTag::OrderAccepted)
                pub.notify_order_accepted(id, side, price, qty);
            pub.process(ev);
        }
        pub.flush_pending();
    };

    add_order(1, Side::Buy,  100, 10);
    add_order(2, Side::Buy,  100, 20);
    add_order(3, Side::Sell, 105, 15);

    auto snap = pub.snapshot();

    // Publisher's snapshot should match ground truth
    auto truth = ground_truth_l2(engine.book());
    REQUIRE(snap.bids.size() == truth.bids.size());
    REQUIRE(snap.asks.size() == truth.asks.size());

    if (!snap.bids.empty() && !truth.bids.empty()) {
        REQUIRE(snap.bids[0].price == truth.bids[0].price);
        REQUIRE(snap.bids[0].qty   == truth.bids[0].qty);
    }
    if (!snap.asks.empty() && !truth.asks.empty()) {
        REQUIRE(snap.asks[0].price == truth.asks[0].price);
        REQUIRE(snap.asks[0].qty   == truth.asks[0].qty);
    }
}

// ── 5. Public Trade Feed (Task 3.2) ──────────────────────────────────────────

TEST_CASE("Public Trade Feed: single match emits exact public trade", "[trade][feed]") {
    Engine engine("tmp_trade_test.log");
    L2Publisher pub;

    // Resting maker: SELL 10 @ 100
    Order maker{};
    maker.id = 1; maker.side = Side::Sell; maker.type = OrderType::Limit;
    maker.status = OrderStatus::Accepted; maker.price = 100; maker.quantity = 10;
    VectorSink s1;
    engine.add(maker, s1);
    for (auto& e : s1.events) {
        if (e.tag() == EventTag::OrderAccepted) pub.notify_order_accepted(1, Side::Sell, 100, 10);
        pub.process(e);
    }
    pub.flush_pending();
    REQUIRE(pub.trades().empty());

    // Incoming taker: BUY 10 @ 100 (aggressor is Buy)
    Order taker{};
    taker.id = 2; taker.side = Side::Buy; taker.type = OrderType::Limit;
    taker.status = OrderStatus::Accepted; taker.price = 100; taker.quantity = 10;
    VectorSink s2;
    engine.add(taker, s2);
    for (auto& e : s2.events) {
        if (e.tag() == EventTag::OrderAccepted) pub.notify_order_accepted(2, Side::Buy, 100, 10);
        pub.process(e);
    }
    pub.flush_pending();

    REQUIRE(pub.trades().size() == 1);
    const auto& trade = pub.trades()[0];
    REQUIRE(trade.price == 100);
    REQUIRE(trade.quantity == 10);
    REQUIRE(trade.aggressor_side == Side::Buy);
    REQUIRE(trade.timestamp > 0);
}

TEST_CASE("Public Trade Feed: aggressor side correctly identifies taker", "[trade][feed]") {
    Engine engine("tmp_aggressor_test.log");
    L2Publisher pub;

    // Resting maker: BUY 20 @ 99
    Order maker{};
    maker.id = 1; maker.side = Side::Buy; maker.type = OrderType::Limit;
    maker.status = OrderStatus::Accepted; maker.price = 99; maker.quantity = 20;
    VectorSink s1;
    engine.add(maker, s1);
    for (auto& e : s1.events) {
        if (e.tag() == EventTag::OrderAccepted) pub.notify_order_accepted(1, Side::Buy, 99, 20);
        pub.process(e);
    }
    pub.flush_pending();

    // Incoming taker: SELL 15 @ 99 (aggressor is Sell)
    Order taker{};
    taker.id = 2; taker.side = Side::Sell; taker.type = OrderType::Limit;
    taker.status = OrderStatus::Accepted; taker.price = 99; taker.quantity = 15;
    VectorSink s2;
    engine.add(taker, s2);
    for (auto& e : s2.events) {
        if (e.tag() == EventTag::OrderAccepted) pub.notify_order_accepted(2, Side::Sell, 99, 15);
        pub.process(e);
    }
    pub.flush_pending();

    REQUIRE(pub.trades().size() == 1);
    const auto& trade = pub.trades()[0];
    REQUIRE(trade.price == 99);
    REQUIRE(trade.quantity == 15);
    REQUIRE(trade.aggressor_side == Side::Sell);
}

TEST_CASE("Public Trade Feed: multi-maker sweep emits ordered public trades", "[trade][feed]") {
    Engine engine("tmp_sweep_trade_test.log");
    L2Publisher pub;

    // Place 3 maker asks: @100 (qty 5), @101 (qty 10), @102 (qty 15)
    for (int i = 0; i < 3; ++i) {
        Order m{};
        m.id = 10 + i; m.side = Side::Sell; m.type = OrderType::Limit;
        m.status = OrderStatus::Accepted; m.price = 100 + i; m.quantity = (i + 1) * 5;
        VectorSink s;
        engine.add(m, s);
        for (auto& e : s.events) {
            if (e.tag() == EventTag::OrderAccepted) pub.notify_order_accepted(m.id, Side::Sell, m.price, m.quantity);
            pub.process(e);
        }
        pub.flush_pending();
    }
    REQUIRE(pub.trades().empty());

    // Taker sweeps @100 (5), @101 (10), and partial @102 (5) with qty=20
    Order taker{};
    taker.id = 99; taker.side = Side::Buy; taker.type = OrderType::Limit;
    taker.status = OrderStatus::Accepted; taker.price = 105; taker.quantity = 20;
    VectorSink st;
    engine.add(taker, st);
    for (auto& e : st.events) {
        if (e.tag() == EventTag::OrderAccepted) pub.notify_order_accepted(99, Side::Buy, 105, 20);
        pub.process(e);
    }
    pub.flush_pending();

    REQUIRE(pub.trades().size() == 3);
    REQUIRE(pub.trades()[0].price == 100);
    REQUIRE(pub.trades()[0].quantity == 5);
    REQUIRE(pub.trades()[0].aggressor_side == Side::Buy);

    REQUIRE(pub.trades()[1].price == 101);
    REQUIRE(pub.trades()[1].quantity == 10);
    REQUIRE(pub.trades()[1].aggressor_side == Side::Buy);

    REQUIRE(pub.trades()[2].price == 102);
    REQUIRE(pub.trades()[2].quantity == 5);
    REQUIRE(pub.trades()[2].aggressor_side == Side::Buy);
}

TEST_CASE("Public Trade Feed: strict sanitization boundary - no participant IDs", "[trade][privacy]") {
    // Compile-time assertions ensure PublicTrade and BBO cannot contain private fields
    static_assert(sizeof(PublicTrade) == 40);
    static_assert(std::is_trivially_copyable_v<PublicTrade>);

    PublicTrade t{
        .seq = 1,
        .timestamp = 1000,
        .price = 150,
        .quantity = 50,
        .aggressor_side = Side::Buy,
        ._pad = {}
    };

    REQUIRE(t.seq == 1);
    REQUIRE(t.price == 150);
    REQUIRE(t.quantity == 50);
    REQUIRE(t.aggressor_side == Side::Buy);
}

// ── 6. Best Bid / Offer (BBO) (Task 3.2) ──────────────────────────────────────

TEST_CASE("BBO: empty book has nullopt BBO", "[bbo]") {
    L2Publisher pub;
    BBO bbo = pub.bbo();

    REQUIRE(bbo.is_empty());
    REQUIRE_FALSE(bbo.has_bid());
    REQUIRE_FALSE(bbo.has_ask());
    REQUIRE_FALSE(bbo.is_two_sided());
    REQUIRE_FALSE(bbo.spread().has_value());
    REQUIRE_FALSE(bbo.mid_price().has_value());
}

TEST_CASE("BBO: single-sided and two-sided book metrics", "[bbo]") {
    Engine engine("tmp_bbo_test.log");
    L2Publisher pub;

    auto add = [&](OrderId id, Side side, Price price, Quantity qty) {
        Order o{};
        o.id = id; o.side = side; o.type = OrderType::Limit;
        o.status = OrderStatus::Accepted; o.price = price; o.quantity = qty;
        VectorSink s;
        engine.add(o, s);
        for (auto& e : s.events) {
            if (e.tag() == EventTag::OrderAccepted) pub.notify_order_accepted(id, side, price, qty);
            pub.process(e);
        }
        pub.flush_pending();
    };

    // Add bid @100 qty=10
    add(1, Side::Buy, 100, 10);
    auto bbo1 = pub.bbo();
    REQUIRE(bbo1.has_bid());
    REQUIRE_FALSE(bbo1.has_ask());
    REQUIRE(bbo1.bid_price == 100);
    REQUIRE(bbo1.bid_qty == 10);
    REQUIRE_FALSE(bbo1.spread().has_value());

    // Add ask @104 qty=20
    add(2, Side::Sell, 104, 20);
    auto bbo2 = pub.bbo();
    REQUIRE(bbo2.is_two_sided());
    REQUIRE(bbo2.bid_price == 100);
    REQUIRE(bbo2.bid_qty == 10);
    REQUIRE(bbo2.ask_price == 104);
    REQUIRE(bbo2.ask_qty == 20);
    REQUIRE(bbo2.spread() == 4);
    REQUIRE(bbo2.mid_price() == 102.0);

    // Add higher bid @101 qty=5 -> BBO updates
    add(3, Side::Buy, 101, 5);
    auto bbo3 = pub.bbo();
    REQUIRE(bbo3.bid_price == 101);
    REQUIRE(bbo3.bid_qty == 5);
    REQUIRE(bbo3.ask_price == 104);
    REQUIRE(bbo3.spread() == 3);
    REQUIRE(bbo3.mid_price() == 102.5);

    // Add same price bid @101 qty=8 -> BBO qty increases
    add(4, Side::Buy, 101, 8);
    auto bbo4 = pub.bbo();
    REQUIRE(bbo4.bid_price == 101);
    REQUIRE(bbo4.bid_qty == 13); // 5 + 8
    REQUIRE(bbo4.spread() == 3);

    // Cancel order 3 (5 of the 13 @ 101) -> BBO qty reduces
    {
        VectorSink sc;
        engine.cancel(3, INVALID_USER_ID, sc);
        for (auto& e : sc.events) pub.process(e);
        pub.flush_pending();
    }
    auto bbo5 = pub.bbo();
    REQUIRE(bbo5.bid_price == 101);
    REQUIRE(bbo5.bid_qty == 8);

    // Cancel order 4 (remaining @ 101) -> BBO drops back to 100
    {
        VectorSink sc;
        engine.cancel(4, INVALID_USER_ID, sc);
        for (auto& e : sc.events) pub.process(e);
        pub.flush_pending();
    }
    auto bbo6 = pub.bbo();
    REQUIRE(bbo6.bid_price == 100);
    REQUIRE(bbo6.bid_qty == 10);
    REQUIRE(bbo6.spread() == 4);
}

// ── 7. MarketDataFeed Pipeline ────────────────────────────────────────────────

TEST_CASE("MarketDataFeed: dispatches trades, BBO updates, and L2 deltas", "[market_data][feed]") {
    Engine engine("tmp_feed_test.log");
    MarketDataFeed feed;

    std::vector<PublicTrade> received_trades;
    std::vector<BBO>         received_bbos;
    std::vector<L2Delta>     received_deltas;

    auto on_trade = [&](const PublicTrade& t) { received_trades.push_back(t); };
    auto on_bbo   = [&](const BBO& b) { received_bbos.push_back(b); };
    auto on_l2    = [&](const L2Update& u) {
        for (auto& d : u) received_deltas.push_back(d);
    };

    // Add resting Sell order: 10 @ 100
    Order o1{};
    o1.id = 1; o1.side = Side::Sell; o1.type = OrderType::Limit;
    o1.status = OrderStatus::Accepted; o1.price = 100; o1.quantity = 10;
    VectorSink s1;
    engine.add(o1, s1);
    for (auto& e : s1.events) {
        if (e.tag() == EventTag::OrderAccepted) feed.notify_order_accepted(1, Side::Sell, 100, 10);
        feed.process(e, on_trade, on_bbo, on_l2);
    }
    feed.flush_pending(on_bbo, on_l2);

    REQUIRE(received_trades.empty());
    REQUIRE(received_bbos.size() == 1);
    REQUIRE(received_bbos.back().ask_price == 100);
    REQUIRE(received_bbos.back().ask_qty == 10);
    REQUIRE(received_deltas.size() == 1);

    // Incoming Buy order matches: 10 @ 100
    Order o2{};
    o2.id = 2; o2.side = Side::Buy; o2.type = OrderType::Limit;
    o2.status = OrderStatus::Accepted; o2.price = 100; o2.quantity = 10;
    VectorSink s2;
    engine.add(o2, s2);
    for (auto& e : s2.events) {
        if (e.tag() == EventTag::OrderAccepted) feed.notify_order_accepted(2, Side::Buy, 100, 10);
        feed.process(e, on_trade, on_bbo, on_l2);
    }
    feed.flush_pending(on_bbo, on_l2);

    REQUIRE(received_trades.size() == 1);
    REQUIRE(received_trades.back().price == 100);
    REQUIRE(received_trades.back().quantity == 10);
    REQUIRE(received_trades.back().aggressor_side == Side::Buy);

    // Book is now empty
    REQUIRE(feed.bbo().is_empty());
}

// ── 8. Randomized BBO and Trade Equivalence ───────────────────────────────────

TEST_CASE("MarketDataFeed: randomized equivalence for BBO and trades", "[bbo][trade][fuzz]") {
    constexpr size_t NUM_SEEDS = 25;
    constexpr size_t OPS_PER_SEED = 200;

    for (uint64_t seed = 1; seed <= NUM_SEEDS; ++seed) {
        CommandGenerator gen(seed * 4321, 95, 105);
        auto sequence = gen.generate(OPS_PER_SEED);

        Engine engine("tmp_bbo_fuzz.log");
        L2Publisher pub;
        size_t expected_total_fills = 0;

        for (size_t i = 0; i < sequence.size(); ++i) {
            const auto& cmd = sequence[i];
            VectorSink sink;

            if (cmd.kind == CommandKind::LimitOrder || cmd.kind == CommandKind::MarketOrder) {
                Order o{};
                o.id = cmd.id; o.side = cmd.side;
                o.type = (cmd.kind == CommandKind::LimitOrder) ? OrderType::Limit : OrderType::Market;
                o.status = OrderStatus::Accepted; o.price = cmd.price; o.quantity = cmd.quantity;
                o.timestamp = cmd.timestamp;
                auto res = engine.add(o, sink);
                expected_total_fills += res.fills.size();
            } else {
                engine.cancel(cmd.id, INVALID_USER_ID, sink);
            }

            for (const auto& ev : sink.events) {
                if (ev.tag() == EventTag::OrderAccepted &&
                    (cmd.kind == CommandKind::LimitOrder || cmd.kind == CommandKind::MarketOrder)) {
                    pub.notify_order_accepted(cmd.id, cmd.side, cmd.price, cmd.quantity);
                }
                pub.process(ev);
            }
            pub.flush_pending();

            // Verify BBO matches engine book's true top of book
            BBO bbo = pub.bbo();

            auto book_bid = engine.book().best_bid();
            if (book_bid.has_value()) {
                REQUIRE(bbo.bid_price == book_bid);
                REQUIRE(bbo.bid_qty == engine.book().bid_level_qty(*book_bid));
            } else {
                REQUIRE_FALSE(bbo.has_bid());
            }

            auto book_ask = engine.book().best_ask();
            if (book_ask.has_value()) {
                REQUIRE(bbo.ask_price == book_ask);
                REQUIRE(bbo.ask_qty == engine.book().ask_level_qty(*book_ask));
            } else {
                REQUIRE_FALSE(bbo.has_ask());
            }
        }

        // Verify every fill produced exactly one public trade
        REQUIRE(pub.trades().size() == expected_total_fills);
    }
}

