#pragma once

#include <cstdint>

namespace orderbook {

using OrderId   = uint64_t;
using Price     = int64_t;
using Quantity  = uint64_t;
using Timestamp = uint64_t; 

// Sentinel for invalid/null OrderId
inline constexpr OrderId INVALID_ORDER_ID = 0;

enum class Side : uint8_t {
    Buy  = 0,
    Sell = 1
};

enum class OrderType : uint8_t {
    Limit  = 0,
    Market = 1
};

enum class OrderStatus : uint8_t {
    Accepted         = 0,
    PartiallyFilled  = 1,
    Filled           = 2,
    Cancelled        = 3
};

// Utility — returns the opposite side. Used heavily in matching logic.
inline Side opposite(Side s) {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

} // namespace orderbook