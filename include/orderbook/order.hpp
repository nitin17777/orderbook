#pragma once

#include "orderbook/types.hpp"
#include <type_traits>

namespace orderbook {

// Memory layout conscious design:
// 7 x 8-byte uint64_t/int64_t fields (56 bytes)
// 3 x 1-byte enums (3 bytes)
// 5 bytes explicit padding (5 bytes)
// Total size: exactly 64 bytes (1 cache line on x86-64 / ARM64).
// Cache-line alignment ensures an Order never spans two cache lines,
// maximizing L1D cache hit rates and avoiding split-cache-line fetches.
struct Order {
    OrderId       id;
    UserId        user_id;
    ClientOrderId client_order_id;
    Price         price;     // ignored for Market orders
    Quantity      quantity;  // original quantity submitted
    Quantity      filled;    // how much has been matched so far
    Timestamp     timestamp; // arrival time — determines FIFO priority
    Side          side;
    OrderType     type;
    OrderStatus   status;
    TimeInForce   tif;
    uint8_t       reserved[4]; // explicit pad to 64 bytes

    // Convenience: how much is still open
    Quantity open_quantity() const {
        return quantity - filled;
    }

    bool is_terminal() const {
        return status == OrderStatus::Filled ||
               status == OrderStatus::Cancelled;
    }

    bool operator==(const Order& other) const = default;
};

static_assert(sizeof(Order) == 64, "Order struct must be exactly 64 bytes (1 cache line)");
static_assert(std::is_trivial_v<Order>, "Order must be trivial for union embedding and POD logging");
static_assert(std::is_standard_layout_v<Order>, "Order must be standard layout");

} // namespace orderbook