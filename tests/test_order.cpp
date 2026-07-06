#include <catch2/catch_test_macros.hpp>
#include "orderbook/book.hpp"

using namespace orderbook;

// Helper to build a limit order cleanly
Order make_limit(OrderId id, Side side, Price price, Quantity qty, Timestamp ts = 0) {
    Order o{};
    o.id        = id;
    o.side      = side;
    o.type      = OrderType::Limit;
    o.status    = OrderStatus::Accepted;
    o.price     = price;
    o.quantity  = qty;
    o.filled    = 0;
    o.timestamp = ts;
    return o;
}

Order make_market(OrderId id, Side side, Quantity qty) {
    Order o{};
    o.id        = id;
    o.side      = side;
    o.type      = OrderType::Market;
    o.status    = OrderStatus::Accepted;
    o.price     = 0;
    o.quantity  = qty;
    o.filled    = 0;
    o.timestamp = 0;
    return o;
}

// ── Basic resting ────────────────────────────────────────────────────────────

TEST_CASE("Limit buy rests in book when no asks", "[book]") {
    OrderBook book;
    auto fills = book.add(make_limit(1, Side::Buy, 100, 10));
    REQUIRE(fills.empty());
    REQUIRE(book.best_bid() == 100);
    REQUIRE(book.order_count() == 1);
}

TEST_CASE("Limit sell rests in book when no bids", "[book]") {
    OrderBook book;
    auto fills = book.add(make_limit(1, Side::Sell, 105, 10));
    REQUIRE(fills.empty());
    REQUIRE(book.best_ask() == 105);
    REQUIRE(book.order_count() == 1);
}

// ── Basic matching ───────────────────────────────────────────────────────────

TEST_CASE("Exact match between buy and sell", "[matching]") {
    OrderBook book;
    book.add(make_limit(1, Side::Sell, 100, 10));

    auto fills = book.add(make_limit(2, Side::Buy, 100, 10));

    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].maker_id  == 1);
    REQUIRE(fills[0].taker_id  == 2);
    REQUIRE(fills[0].price     == 100);
    REQUIRE(fills[0].quantity  == 10);

    REQUIRE(book.order_count() == 0); // both fully filled
    REQUIRE_FALSE(book.best_bid().has_value());
    REQUIRE_FALSE(book.best_ask().has_value());
}

TEST_CASE("Partial fill — remainder rests", "[matching]") {
    OrderBook book;
    book.add(make_limit(1, Side::Sell, 100, 5));

    auto fills = book.add(make_limit(2, Side::Buy, 100, 10));

    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].quantity == 5);

    // maker fully filled, taker partially filled and resting
    REQUIRE(book.order_count() == 1);
    REQUIRE(book.best_bid() == 100);
}

TEST_CASE("Sweep multiple price levels", "[matching]") {
    OrderBook book;
    book.add(make_limit(1, Side::Sell, 100, 5));
    book.add(make_limit(2, Side::Sell, 101, 5));
    book.add(make_limit(3, Side::Sell, 102, 5));

    // Large buy sweeps all three levels
    auto fills = book.add(make_limit(4, Side::Buy, 105, 15));

    REQUIRE(fills.size() == 3);
    REQUIRE(fills[0].price == 100);
    REQUIRE(fills[1].price == 101);
    REQUIRE(fills[2].price == 102);
    REQUIRE(book.order_count() == 0);
}

TEST_CASE("FIFO priority at same price level", "[matching]") {
    OrderBook book;
    book.add(make_limit(1, Side::Sell, 100, 5, 1000)); // arrives first
    book.add(make_limit(2, Side::Sell, 100, 5, 2000)); // arrives second

    auto fills = book.add(make_limit(3, Side::Buy, 100, 5));

    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].maker_id == 1); // order 1 matched first
}

// ── Market orders ────────────────────────────────────────────────────────────

TEST_CASE("Market order fills against best ask", "[market]") {
    OrderBook book;
    book.add(make_limit(1, Side::Sell, 100, 10));

    auto fills = book.add(make_market(2, Side::Buy, 10));
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].quantity == 10);
    REQUIRE(book.order_count() == 0);
}

TEST_CASE("Market order against empty book generates no fills", "[market]") {
    OrderBook book;
    auto fills = book.add(make_market(1, Side::Buy, 10));
    REQUIRE(fills.empty());
    REQUIRE(book.order_count() == 0); // market order must not rest
}

TEST_CASE("Market order partial fill — remainder cancelled not rested", "[market]") {
    OrderBook book;
    book.add(make_limit(1, Side::Sell, 100, 3));

    auto fills = book.add(make_market(2, Side::Buy, 10));
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].quantity == 3);
    REQUIRE(book.order_count() == 0); // remainder must not rest
}

// ── Cancel ───────────────────────────────────────────────────────────────────

TEST_CASE("Cancel removes order from book", "[cancel]") {
    OrderBook book;
    book.add(make_limit(1, Side::Buy, 100, 10));
    REQUIRE(book.order_count() == 1);

    bool cancelled = book.cancel(1);
    REQUIRE(cancelled == true);
    REQUIRE(book.order_count() == 0);
    REQUIRE_FALSE(book.best_bid().has_value());
}

TEST_CASE("Cancel unknown id is silent no-op", "[cancel]") {
    OrderBook book;
    bool cancelled = book.cancel(999);
    REQUIRE(cancelled == false);
}

TEST_CASE("Cancel already filled order is silent no-op", "[cancel]") {
    OrderBook book;
    book.add(make_limit(1, Side::Sell, 100, 10));
    book.add(make_limit(2, Side::Buy,  100, 10)); // fully fills order 1

    bool cancelled = book.cancel(1); // order 1 already gone
    REQUIRE(cancelled == false);
}