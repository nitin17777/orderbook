#include <catch2/catch_test_macros.hpp>
#include "orderbook/engine.hpp"

#include <filesystem>
#include <cstdio>

using namespace orderbook;

// Helper — removes temp log file after each test
struct TempLog {
    std::string path;
    explicit TempLog(const std::string& p) : path(p) {}
    ~TempLog() { std::filesystem::remove(path); }
};

static Order make_limit(OrderId id, Side side, Price price, Quantity qty,
                          UserId user_id = INVALID_USER_ID,
                          ClientOrderId client_id = INVALID_CLIENT_ORDER_ID) {
    Order o{};
    o.id              = id;
    o.user_id         = user_id;
    o.client_order_id = client_id;
    o.side            = side;
    o.type            = OrderType::Limit;
    o.status          = OrderStatus::Accepted;
    o.price           = price;
    o.quantity        = qty;
    o.filled          = 0;
    o.timestamp       = id;
    return o;
}

static Order make_market(OrderId id, Side side, Quantity qty,
                           UserId user_id = INVALID_USER_ID,
                           ClientOrderId client_id = INVALID_CLIENT_ORDER_ID) {
    Order o{};
    o.id              = id;
    o.user_id         = user_id;
    o.client_order_id = client_id;
    o.side            = side;
    o.type            = OrderType::Market;
    o.status          = OrderStatus::Accepted;
    o.price           = 0;
    o.quantity        = qty;
    o.filled          = 0;
    o.timestamp       = id;
    return o;
}

// ── Memory layout verification ────────────────────────────────────────────────

TEST_CASE("Order struct layout is 64 bytes (1 cache line)", "[order][layout]") {
    REQUIRE(sizeof(Order) == 64);
    REQUIRE(std::is_trivially_copyable_v<Order>);
}

// ── Basic engine operation ────────────────────────────────────────────────────

TEST_CASE("Engine: add and cancel logged correctly", "[engine]") {
    TempLog tmp("test_basic.log");
    {
        Engine engine(tmp.path);
        auto r1 = engine.add(make_limit(1, Side::Buy,  100, 10));
        REQUIRE(r1.accepted);
        auto r2 = engine.add(make_limit(2, Side::Sell, 105, 10));
        REQUIRE(r2.accepted);
        auto r3 = engine.cancel(1);
        REQUIRE(r3.accepted);
        engine.flush();
    }

    auto commands = EventLog::read_all(tmp.path);
    REQUIRE(commands.size() == 3);
    REQUIRE(commands[0].type == CommandType::AddOrder);
    REQUIRE(commands[0].order.id == 1);
    REQUIRE(commands[1].type == CommandType::AddOrder);
    REQUIRE(commands[1].order.id == 2);
    REQUIRE(commands[2].type == CommandType::CancelOrder);
    REQUIRE(commands[2].cancel_id == 1);
}

// ── Input Validation & Rejection ──────────────────────────────────────────────

TEST_CASE("Engine: rejects zero quantity and leaves state untouched", "[engine][validation]") {
    TempLog tmp("test_invalid_qty.log");
    Engine engine(tmp.path);

    // Initial valid order
    REQUIRE(engine.add(make_limit(1, Side::Buy, 100, 10)).accepted);

    // Zero quantity limit order
    auto res_limit = engine.add(make_limit(2, Side::Buy, 100, 0));
    REQUIRE_FALSE(res_limit.accepted);
    REQUIRE(res_limit.reason == RejectReason::InvalidQuantity);
    REQUIRE(res_limit.fills.empty());

    // Zero quantity market order
    auto res_mkt = engine.add(make_market(3, Side::Sell, 0));
    REQUIRE_FALSE(res_mkt.accepted);
    REQUIRE(res_mkt.reason == RejectReason::InvalidQuantity);

    // State must be completely unchanged
    REQUIRE(engine.book().order_count() == 1);
    REQUIRE(engine.book().best_bid() == 100);

    // EventLog must contain only the first valid order
    engine.flush();
    auto commands = EventLog::read_all(tmp.path);
    REQUIRE(commands.size() == 1);
    REQUIRE(commands[0].order.id == 1);
}

TEST_CASE("Engine: rejects non-positive limit prices and leaves state untouched", "[engine][validation]") {
    TempLog tmp("test_invalid_price.log");
    Engine engine(tmp.path);

    auto res_zero = engine.add(make_limit(1, Side::Buy, 0, 10));
    REQUIRE_FALSE(res_zero.accepted);
    REQUIRE(res_zero.reason == RejectReason::InvalidPrice);

    auto res_neg = engine.add(make_limit(2, Side::Sell, -50, 10));
    REQUIRE_FALSE(res_neg.accepted);
    REQUIRE(res_neg.reason == RejectReason::InvalidPrice);

    // Book remains empty
    REQUIRE(engine.book().order_count() == 0);
    REQUIRE_FALSE(engine.book().best_bid().has_value());
    REQUIRE_FALSE(engine.book().best_ask().has_value());

    engine.flush();
    auto commands = EventLog::read_all(tmp.path);
    REQUIRE(commands.empty());
}

TEST_CASE("Engine: rejects duplicate internal OrderId", "[engine][validation]") {
    TempLog tmp("test_dup_order_id.log");
    Engine engine(tmp.path);

    auto r1 = engine.add(make_limit(100, Side::Buy, 100, 10));
    REQUIRE(r1.accepted);

    // Attempt to submit another order with the same explicit OrderId
    auto r2 = engine.add(make_limit(100, Side::Sell, 105, 5));
    REQUIRE_FALSE(r2.accepted);
    REQUIRE(r2.reason == RejectReason::DuplicateOrderId);

    // Book has only 1 order
    REQUIRE(engine.book().order_count() == 1);
    REQUIRE(engine.book().best_bid() == 100);
}

TEST_CASE("Engine: rejects duplicate ClientOrderId per user", "[engine][validation]") {
    TempLog tmp("test_dup_client_id.log");
    Engine engine(tmp.path);

    UserId user_a = 101;
    UserId user_b = 102;
    ClientOrderId clord_1 = 5001;

    // User A submits order with client_order_id 5001
    auto r1 = engine.add(make_limit(1, Side::Buy, 100, 10, user_a, clord_1));
    REQUIRE(r1.accepted);

    // User A submits duplicate client_order_id 5001 -> rejected
    auto r2 = engine.add(make_limit(2, Side::Buy, 99, 10, user_a, clord_1));
    REQUIRE_FALSE(r2.accepted);
    REQUIRE(r2.reason == RejectReason::DuplicateClientOrderId);

    // User B submits same client_order_id 5001 -> accepted (isolated user namespace)
    auto r3 = engine.add(make_limit(3, Side::Sell, 105, 10, user_b, clord_1));
    REQUIRE(r3.accepted);

    REQUIRE(engine.book().order_count() == 2);
}

TEST_CASE("Engine: order ID assignment when INVALID_ORDER_ID (0) is passed", "[engine][identity]") {
    TempLog tmp("test_id_assign.log");
    Engine engine(tmp.path);

    auto r1 = engine.add(make_limit(INVALID_ORDER_ID, Side::Buy, 100, 10));
    REQUIRE(r1.accepted);

    auto r2 = engine.add(make_limit(INVALID_ORDER_ID, Side::Sell, 105, 10));
    REQUIRE(r2.accepted);

    engine.flush();
    auto commands = EventLog::read_all(tmp.path);
    REQUIRE(commands.size() == 2);
    REQUIRE(commands[0].order.id == 1);
    REQUIRE(commands[1].order.id == 2);
}

TEST_CASE("Engine: cancel validation (unknown order and non-owner)", "[engine][cancel]") {
    TempLog tmp("test_cancel_validation.log");
    Engine engine(tmp.path);

    UserId owner = 42;
    UserId intruder = 99;

    auto r1 = engine.add(make_limit(1, Side::Buy, 100, 10, owner, 1001));
    REQUIRE(r1.accepted);

    // Cancel unknown order
    auto c_unknown = engine.cancel(999);
    REQUIRE_FALSE(c_unknown.accepted);
    REQUIRE(c_unknown.reason == RejectReason::UnknownOrder);

    // Cancel by non-owner
    auto c_unauth = engine.cancel(1, intruder);
    REQUIRE_FALSE(c_unauth.accepted);
    REQUIRE(c_unauth.reason == RejectReason::Unauthorized);
    REQUIRE(engine.book().order_count() == 1); // Order still resting

    // Cancel by owner
    auto c_owner = engine.cancel(1, owner);
    REQUIRE(c_owner.accepted);
    REQUIRE(engine.book().order_count() == 0);
}

TEST_CASE("Engine: cancel by ClientOrderId", "[engine][cancel]") {
    TempLog tmp("test_cancel_client_id.log");
    Engine engine(tmp.path);

    UserId user = 10;
    ClientOrderId clord = 777;

    auto r = engine.add(make_limit(1, Side::Buy, 100, 10, user, clord));
    REQUIRE(r.accepted);
    REQUIRE(engine.lookup_client_order(user, clord) == 1);

    auto c_res = engine.cancel_by_client_id(user, clord);
    REQUIRE(c_res.accepted);
    REQUIRE(engine.book().order_count() == 0);
    REQUIRE(engine.lookup_client_order(user, clord) == INVALID_ORDER_ID);

    // Re-cancelling is unknown
    auto c_again = engine.cancel_by_client_id(user, clord);
    REQUIRE_FALSE(c_again.accepted);
    REQUIRE(c_again.reason == RejectReason::UnknownOrder);
}

// ── Replay fidelity ───────────────────────────────────────────────────────────

TEST_CASE("Engine: replay produces identical book state", "[engine]") {
    TempLog tmp("test_replay.log");

    std::optional<Price> original_best_bid;
    std::optional<Price> original_best_ask;
    std::size_t          original_order_count;

    {
        Engine engine(tmp.path);
        engine.add(make_limit(1, Side::Buy,  100, 10));
        engine.add(make_limit(2, Side::Buy,   99,  5));
        engine.add(make_limit(3, Side::Sell, 101, 10));
        engine.add(make_limit(4, Side::Sell, 102,  5));
        engine.cancel(2); // cancel the 99 bid
        engine.flush();

        original_best_bid    = engine.book().best_bid();
        original_best_ask    = engine.book().best_ask();
        original_order_count = engine.book().order_count();
    }

    auto result = Engine::replay(tmp.path);

    REQUIRE(result.book.best_bid()    == original_best_bid);
    REQUIRE(result.book.best_ask()    == original_best_ask);
    REQUIRE(result.book.order_count() == original_order_count);
}

TEST_CASE("Engine: replay preserves fills", "[engine]") {
    TempLog tmp("test_replay_fills.log");

    std::vector<Fill> original_fills;
    {
        Engine engine(tmp.path);
        engine.add(make_limit(1, Side::Sell, 100, 10));
        auto res = engine.add(make_limit(2, Side::Buy, 100, 10));
        original_fills = res.fills;
        engine.flush();
    }

    auto result = Engine::replay(tmp.path);

    REQUIRE(result.fills.size() == original_fills.size());
    REQUIRE(result.fills[0].maker_id  == original_fills[0].maker_id);
    REQUIRE(result.fills[0].taker_id  == original_fills[0].taker_id);
    REQUIRE(result.fills[0].price     == original_fills[0].price);
    REQUIRE(result.fills[0].quantity  == original_fills[0].quantity);
}

TEST_CASE("Engine: empty log replays to empty book", "[engine]") {
    TempLog tmp("test_empty.log");
    {
        Engine engine(tmp.path);
        engine.flush();
    }

    auto result = Engine::replay(tmp.path);
    REQUIRE(result.book.order_count() == 0);
    REQUIRE(result.fills.empty());
}

TEST_CASE("Engine: replay after partial fills is correct", "[engine]") {
    TempLog tmp("test_partial.log");
    {
        Engine engine(tmp.path);
        engine.add(make_limit(1, Side::Sell, 100, 5));
        engine.add(make_limit(2, Side::Buy,  100, 10)); // partial fill — 5 rests
        engine.flush();
    }

    auto result = Engine::replay(tmp.path);
    REQUIRE(result.book.order_count() == 1);
    REQUIRE(result.book.best_bid() == 100);
    REQUIRE(result.fills.size() == 1);
    REQUIRE(result.fills[0].quantity == 5);
}

// ── Engine Time-in-Force Event Tests ─────────────────────────────────────────

static Order make_ioc_engine(OrderId id, Side side, Price price, Quantity qty) {
    Order o = make_limit(id, side, price, qty);
    o.tif   = TimeInForce::IOC;
    return o;
}

static Order make_fok_engine(OrderId id, Side side, Price price, Quantity qty) {
    Order o = make_limit(id, side, price, qty);
    o.tif   = TimeInForce::FOK;
    return o;
}

TEST_CASE("Engine: IOC partial fill emits OrderAccepted, Fill, and OrderCancelled for remainder", "[engine][tif][ioc]") {
    TempLog tmp("test_ioc_events.log");
    Engine engine(tmp.path);
    VectorSink sink;

    engine.add(make_limit(1, Side::Sell, 100, 5), sink);

    auto res = engine.add(make_ioc_engine(2, Side::Buy, 100, 15), sink);
    REQUIRE(res.accepted);
    REQUIRE(res.fills.size() == 1);
    REQUIRE(res.fills[0].quantity == 5);

    // Order 1 was 1 event (accepted).
    // Order 2 events should be: OrderAccepted(2), Fill(2, 1, 100, 5), OrderCancelled(2, remaining=10)
    REQUIRE(sink.events.size() == 4);
    REQUIRE(sink.events[1].tag() == EventTag::OrderAccepted);
    REQUIRE(sink.events[1].accepted.order_id == 2);
    REQUIRE(sink.events[1].accepted.tif == TimeInForce::IOC);

    REQUIRE(sink.events[2].tag() == EventTag::Fill);
    REQUIRE(sink.events[2].fill.taker_id == 2);
    REQUIRE(sink.events[2].fill.maker_id == 1);
    REQUIRE(sink.events[2].fill.quantity == 5);

    REQUIRE(sink.events[3].tag() == EventTag::OrderCancelled);
    REQUIRE(sink.events[3].cancelled.order_id == 2);
    REQUIRE(sink.events[3].cancelled.remaining_qty == 10);

    REQUIRE(engine.book().order_count() == 0);
}

TEST_CASE("Engine: FOK insufficient liquidity emits OrderAccepted followed immediately by OrderCancelled", "[engine][tif][fok]") {
    TempLog tmp("test_fok_reject_events.log");
    Engine engine(tmp.path);
    VectorSink sink;

    engine.add(make_limit(1, Side::Sell, 100, 5), sink);

    // Wants 10 lots, only 5 available -> killed
    auto res = engine.add(make_fok_engine(2, Side::Buy, 100, 10), sink);
    REQUIRE(res.accepted);
    REQUIRE(res.fills.empty());

    // Events for Order 2: OrderAccepted(2), OrderCancelled(2, remaining=10)
    REQUIRE(sink.events.size() == 3);
    REQUIRE(sink.events[1].tag() == EventTag::OrderAccepted);
    REQUIRE(sink.events[1].accepted.order_id == 2);
    REQUIRE(sink.events[1].accepted.tif == TimeInForce::FOK);

    REQUIRE(sink.events[2].tag() == EventTag::OrderCancelled);
    REQUIRE(sink.events[2].cancelled.order_id == 2);
    REQUIRE(sink.events[2].cancelled.remaining_qty == 10);

    // Book state untouched
    REQUIRE(engine.book().order_count() == 1);
    REQUIRE(engine.book().best_ask() == 100);
}

TEST_CASE("Engine: FOK full fill emits OrderAccepted and Fills without OrderCancelled", "[engine][tif][fok]") {
    TempLog tmp("test_fok_fill_events.log");
    Engine engine(tmp.path);
    VectorSink sink;

    engine.add(make_limit(1, Side::Sell, 100, 5), sink);
    engine.add(make_limit(2, Side::Sell, 101, 5), sink);

    auto res = engine.add(make_fok_engine(3, Side::Buy, 105, 10), sink);
    REQUIRE(res.accepted);
    REQUIRE(res.fills.size() == 2);

    // Total events:
    // Order 1: OrderAccepted
    // Order 2: OrderAccepted
    // Order 3: OrderAccepted, Fill, Fill (no OrderCancelled!)
    REQUIRE(sink.events.size() == 5);
    REQUIRE(sink.events[2].tag() == EventTag::OrderAccepted);
    REQUIRE(sink.events[2].accepted.order_id == 3);
    REQUIRE(sink.events[3].tag() == EventTag::Fill);
    REQUIRE(sink.events[4].tag() == EventTag::Fill);

    REQUIRE(engine.book().order_count() == 0);
}

TEST_CASE("Engine: Market order partial fill emits OrderCancelled for remainder", "[engine][market]") {
    TempLog tmp("test_market_cancel_events.log");
    Engine engine(tmp.path);
    VectorSink sink;

    engine.add(make_limit(1, Side::Sell, 100, 4), sink);

    auto res = engine.add(make_market(2, Side::Buy, 10), sink);
    REQUIRE(res.accepted);
    REQUIRE(res.fills.size() == 1);
    REQUIRE(res.fills[0].quantity == 4);

    REQUIRE(sink.events.size() == 4);
    REQUIRE(sink.events[1].tag() == EventTag::OrderAccepted);
    REQUIRE(sink.events[2].tag() == EventTag::Fill);
    REQUIRE(sink.events[3].tag() == EventTag::OrderCancelled);
    REQUIRE(sink.events[3].cancelled.order_id == 2);
    REQUIRE(sink.events[3].cancelled.remaining_qty == 6);

    REQUIRE(engine.book().order_count() == 0);
}

TEST_CASE("Engine: Replay with IOC and FOK produces identical state and fills", "[engine][tif][replay]") {
    TempLog tmp("test_tif_replay.log");

    std::vector<Fill> original_fills;
    {
        Engine engine(tmp.path);
        engine.add(make_limit(1, Side::Sell, 100, 10));
        engine.add(make_limit(2, Side::Sell, 105, 10));

        // IOC partial fill
        auto r_ioc = engine.add(make_ioc_engine(3, Side::Buy, 100, 15));
        original_fills.insert(original_fills.end(), r_ioc.fills.begin(), r_ioc.fills.end());

        // FOK that fails (needs 15, only 10 available at 105)
        auto r_fok_fail = engine.add(make_fok_engine(4, Side::Buy, 105, 15));
        original_fills.insert(original_fills.end(), r_fok_fail.fills.begin(), r_fok_fail.fills.end());

        // FOK that succeeds (takes remaining 10 at 105)
        auto r_fok_ok = engine.add(make_fok_engine(5, Side::Buy, 105, 10));
        original_fills.insert(original_fills.end(), r_fok_ok.fills.begin(), r_fok_ok.fills.end());

        engine.flush();
    }

    auto result = Engine::replay(tmp.path);
    REQUIRE(result.book.order_count() == 0);
    REQUIRE(result.fills.size() == original_fills.size());
    for (size_t i = 0; i < original_fills.size(); ++i) {
        REQUIRE(result.fills[i].maker_id == original_fills[i].maker_id);
        REQUIRE(result.fills[i].taker_id == original_fills[i].taker_id);
        REQUIRE(result.fills[i].price == original_fills[i].price);
        REQUIRE(result.fills[i].quantity == original_fills[i].quantity);
    }
}

// ── Task 5.2: Order Modification (Cancel-Replace) and Priority Rules ──────────

TEST_CASE("Engine: modify — reduce quantity retains queue priority", "[engine][modify][priority]") {
    // Three bids at the same price: orders 1, 2, 3 in FIFO order.
    // Reducing order 2's quantity should NOT move it to the back.
    // A sell at 100 for qty=10 should fill order 1 (5) then order 2 (5, partially).
    TempLog tmp("test_modify_retain.log");
    Engine engine(tmp.path);

    engine.add(make_limit(1, Side::Buy, 100, 5));   // queue pos 0
    engine.add(make_limit(2, Side::Buy, 100, 10));  // queue pos 1
    engine.add(make_limit(3, Side::Buy, 100, 10));  // queue pos 2

    // Reduce order 2 from qty=10 to qty=8 at same price — must retain position
    VectorSink sink;
    auto mres = engine.modify(2, 100, 8, INVALID_USER_ID, sink);
    REQUIRE(mres.accepted);
    REQUIRE(mres.fills.empty());  // no match on reduce

    // Verify EventOrderModified was emitted
    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events[0].tag() == EventTag::OrderModified);
    REQUIRE(sink.events[0].modified.order_id == 2);
    REQUIRE(sink.events[0].modified.new_price == 100);
    REQUIRE(sink.events[0].modified.new_quantity == 8);

    // Now send a sell that fills 5+5=10 lots.
    // Should fill order 1 (5) then order 2 (5 of 8) — priority retained.
    VectorSink sink2;
    auto res = engine.add(make_limit(4, Side::Sell, 100, 10), sink2);
    REQUIRE(res.accepted);
    REQUIRE(res.fills.size() == 2);
    REQUIRE(res.fills[0].maker_id == 1);  // order 1 first (FIFO)
    REQUIRE(res.fills[0].quantity == 5);
    REQUIRE(res.fills[1].maker_id == 2);  // order 2 second (retained priority)
    REQUIRE(res.fills[1].quantity == 5);

    // Order 3 (10 lots) and remainder of 2 (3 lots) still resting
    REQUIRE(engine.book().order_count() == 2);
}

TEST_CASE("Engine: modify — increase quantity loses queue priority", "[engine][modify][priority]") {
    // Three bids at 100. Increasing order 1's qty moves it to the back.
    // A sell for 10 should now fill orders 2 and 3 before modified order 1.
    TempLog tmp("test_modify_lose_qty.log");
    Engine engine(tmp.path);

    engine.add(make_limit(1, Side::Buy, 100, 5));   // front
    engine.add(make_limit(2, Side::Buy, 100, 5));
    engine.add(make_limit(3, Side::Buy, 100, 5));

    // Increase order 1 from qty=5 to qty=10 — loses priority, goes to back
    auto mres = engine.modify(1, 100, 10);
    REQUIRE(mres.accepted);
    REQUIRE(mres.fills.empty());

    // Sell 10: should fill order 2 (5) then order 3 (5). Order 1 is now last.
    auto res = engine.add(make_limit(4, Side::Sell, 100, 10));
    REQUIRE(res.accepted);
    REQUIRE(res.fills.size() == 2);
    REQUIRE(res.fills[0].maker_id == 2);
    REQUIRE(res.fills[0].quantity == 5);
    REQUIRE(res.fills[1].maker_id == 3);
    REQUIRE(res.fills[1].quantity == 5);

    // Order 1 (now qty=10) still resting at back
    REQUIRE(engine.book().order_count() == 1);
    REQUIRE(engine.book().best_bid() == 100);
}

TEST_CASE("Engine: modify — price change loses queue priority", "[engine][modify][priority]") {
    // Order 1 is an ask at 105. Order 2 is an ask at 105 behind it.
    // Moving order 1 to price 106 should take it out of the 105 level.
    // A subsequent buy at 105 for qty=10 should fill only order 2.
    TempLog tmp("test_modify_lose_price.log");
    Engine engine(tmp.path);

    engine.add(make_limit(1, Side::Sell, 105, 10));
    engine.add(make_limit(2, Side::Sell, 105, 10));

    // Move order 1 to 106
    auto mres = engine.modify(1, 106, 10);
    REQUIRE(mres.accepted);
    REQUIRE(mres.fills.empty());

    // Verify L2 depths
    REQUIRE(engine.book().ask_level_qty(105) == 10);  // only order 2 remains
    REQUIRE(engine.book().ask_level_qty(106) == 10);  // order 1 moved here

    // Buy at 105 — should fill order 2, not order 1
    auto res = engine.add(make_limit(3, Side::Buy, 105, 10));
    REQUIRE(res.accepted);
    REQUIRE(res.fills.size() == 1);
    REQUIRE(res.fills[0].maker_id == 2);

    REQUIRE(engine.book().order_count() == 1);  // order 1 at 106
    REQUIRE(engine.book().best_ask() == 106);
}

TEST_CASE("Engine: modify — price-crossing triggers immediate fill", "[engine][modify][cross]") {
    // Resting bid at 100. Modify a resting ask from 105 to 100 — it now crosses.
    TempLog tmp("test_modify_cross.log");
    Engine engine(tmp.path);

    engine.add(make_limit(1, Side::Buy,  100, 10));   // resting bid
    engine.add(make_limit(2, Side::Sell, 105, 10));   // resting ask — no cross yet

    REQUIRE(engine.book().order_count() == 2);

    // Modify ask price from 105 down to 100 — should immediately cross
    VectorSink sink;
    auto mres = engine.modify(2, 100, 10, INVALID_USER_ID, sink);
    REQUIRE(mres.accepted);
    REQUIRE(mres.fills.size() == 1);
    REQUIRE(mres.fills[0].taker_id == 2);
    REQUIRE(mres.fills[0].maker_id == 1);
    REQUIRE(mres.fills[0].price    == 100);  // maker's price
    REQUIRE(mres.fills[0].quantity == 10);

    // Both orders fully filled → empty book
    REQUIRE(engine.book().order_count() == 0);

    // Event sequence: OrderModified, Fill
    REQUIRE(sink.events.size() == 2);
    REQUIRE(sink.events[0].tag() == EventTag::OrderModified);
    REQUIRE(sink.events[1].tag() == EventTag::Fill);
}

TEST_CASE("Engine: modify — unknown order is rejected", "[engine][modify][validation]") {
    TempLog tmp("test_modify_unknown.log");
    Engine engine(tmp.path);

    engine.add(make_limit(1, Side::Buy, 100, 10));

    VectorSink sink;
    auto mres = engine.modify(999, 100, 5, INVALID_USER_ID, sink);
    REQUIRE_FALSE(mres.accepted);
    REQUIRE(mres.reason == RejectReason::UnknownOrder);

    // OrderRejected event emitted
    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events[0].tag() == EventTag::OrderRejected);

    // Book unchanged
    REQUIRE(engine.book().order_count() == 1);
}

TEST_CASE("Engine: modify — non-owner is rejected", "[engine][modify][validation]") {
    TempLog tmp("test_modify_unauth.log");
    Engine engine(tmp.path);

    UserId owner = 42;
    UserId intruder = 99;
    engine.add(make_limit(1, Side::Buy, 100, 10, owner, 1001));

    auto mres = engine.modify(1, 100, 5, intruder);
    REQUIRE_FALSE(mres.accepted);
    REQUIRE(mres.reason == RejectReason::Unauthorized);

    REQUIRE(engine.book().order_count() == 1);
}

TEST_CASE("Engine: modify — L2 aggregates remain correct after retain and lose paths", "[engine][modify][l2]") {
    TempLog tmp("test_modify_l2.log");
    Engine engine(tmp.path);

    // Place two bids: 10@100 and 8@100
    engine.add(make_limit(1, Side::Buy, 100, 10));
    engine.add(make_limit(2, Side::Buy, 100, 8));
    REQUIRE(engine.book().bid_level_qty(100) == 18);

    // Reduce order 1 from 10 to 6 — retain path
    engine.modify(1, 100, 6);
    REQUIRE(engine.book().bid_level_qty(100) == 14);  // 6+8

    // Increase order 2 from 8 to 12 — lose path (new qty > old open)
    engine.modify(2, 100, 12);
    REQUIRE(engine.book().bid_level_qty(100) == 18);  // 6+12

    // Move order 1 to 101 — price change
    engine.modify(1, 101, 6);
    REQUIRE(engine.book().bid_level_qty(100) == 12);  // only order 2
    REQUIRE(engine.book().bid_level_qty(101) == 6);
}

TEST_CASE("Engine: modify — replay preserves fills from crossing modify", "[engine][modify][replay]") {
    TempLog tmp("test_modify_replay.log");

    std::vector<Fill> original_fills;
    {
        Engine engine(tmp.path);
        engine.add(make_limit(1, Side::Buy,  100, 10));
        engine.add(make_limit(2, Side::Sell, 105, 10));

        // Retain path — no fills
        engine.modify(1, 100, 7);

        // Price-crossing modify — generates fill
        auto mres = engine.modify(2, 100, 10);
        original_fills.insert(original_fills.end(), mres.fills.begin(), mres.fills.end());

        engine.flush();
    }

    auto result = Engine::replay(tmp.path);
    REQUIRE(result.fills.size() == original_fills.size());
    for (size_t i = 0; i < original_fills.size(); ++i) {
        REQUIRE(result.fills[i].maker_id == original_fills[i].maker_id);
        REQUIRE(result.fills[i].taker_id == original_fills[i].taker_id);
        REQUIRE(result.fills[i].price    == original_fills[i].price);
        REQUIRE(result.fills[i].quantity == original_fills[i].quantity);
    }
}

