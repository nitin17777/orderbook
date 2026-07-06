#pragma once

#include "orderbook/types.hpp"

namespace orderbook {

// Emitted every time two orders match.
// One incoming order can produce multiple fills
// (e.g. a large market order sweeping several resting limit orders).
struct Fill {
    OrderId  taker_id;  // incoming order that triggered the match
    OrderId  maker_id;  // resting order that was already in the book
    Price    price;     // always the maker's price — exchange convention
    Quantity quantity;  // how much was exchanged
};

} // namespace orderbook