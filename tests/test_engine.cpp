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