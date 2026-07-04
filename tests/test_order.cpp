#include <catch2/catch_test_macros.hpp>
#include "orderbook/order.hpp"

TEST_CASE("placeholder works", "[sanity]") {
    REQUIRE(orderbook::placeholder() == 42);
}