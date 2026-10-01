#pragma once

#include <cstdint>

namespace orderbook {

using OrderId       = uint64_t;
using UserId        = uint64_t;
using ClientOrderId = uint64_t;
using Price         = int64_t;
using Quantity      = uint64_t;
using Timestamp     = uint64_t;

// Sentinels for invalid/null IDs
inline constexpr OrderId       INVALID_ORDER_ID        = 0;
inline constexpr UserId        INVALID_USER_ID         = 0;
inline constexpr ClientOrderId INVALID_CLIENT_ORDER_ID = 0;

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

enum class RejectReason : uint8_t {
    None = 0,
    InvalidPrice,
    InvalidQuantity,
    DuplicateOrderId,
    DuplicateClientOrderId,
    UnknownOrder,
    Unauthorized,
    InvalidSide,
    InvalidType
};

inline const char* to_string(RejectReason reason) {
    switch (reason) {
        case RejectReason::None:                   return "None";
        case RejectReason::InvalidPrice:           return "InvalidPrice";
        case RejectReason::InvalidQuantity:        return "InvalidQuantity";
        case RejectReason::DuplicateOrderId:       return "DuplicateOrderId";
        case RejectReason::DuplicateClientOrderId: return "DuplicateClientOrderId";
        case RejectReason::UnknownOrder:           return "UnknownOrder";
        case RejectReason::Unauthorized:           return "Unauthorized";
        case RejectReason::InvalidSide:            return "InvalidSide";
        case RejectReason::InvalidType:            return "InvalidType";
    }
    return "Unknown";
}

// Utility — returns the opposite side. Used heavily in matching logic.
inline Side opposite(Side s) {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

} // namespace orderbook