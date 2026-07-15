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

static Order make_limit(OrderId id, Side side, Price price, Quantity qty) {
    Order o{};
    o.id = id; o.side = side; o.type = OrderType::Limit;
    o.status = OrderStatus::Accepted;
    o.price = price; o.quantity = qty; o.filled = 0; o.timestamp = id;
    return o;
}

// ── Basic engine operation ────────────────────────────────────────────────────

TEST_CASE("Engine: add and cancel logged correctly", "[engine]") {
    TempLog tmp("test_basic.log");
    {
        Engine engine(tmp.path);
        engine.add(make_limit(1, Side::Buy,  100, 10));
        engine.add(make_limit(2, Side::Sell, 105, 10));
        engine.cancel(1);
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

// ── Replay fidelity ───────────────────────────────────────────────────────────

TEST_CASE("Engine: replay produces identical book state", "[engine]") {
    TempLog tmp("test_replay.log");

    // Build a book via Engine and record its state
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

    // Replay the log into a fresh book
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
        auto fills = engine.add(make_limit(2, Side::Buy, 100, 10));
        original_fills = fills;
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
    // order 1 fully filled, order 2 partially filled — 5 qty resting
    REQUIRE(result.book.order_count() == 1);
    REQUIRE(result.book.best_bid() == 100);
    REQUIRE(result.fills.size() == 1);
    REQUIRE(result.fills[0].quantity == 5);
}