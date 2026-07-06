#pragma once

#include "orderbook/types.hpp"

namespace orderbook {

struct Order {
    OrderId    id;
    Side       side;
    OrderType  type;
    OrderStatus status;
    Price      price;     // ignored for Market orders
    Quantity   quantity;  // original quantity submitted
    Quantity   filled;    // how much has been matched so far
    Timestamp  timestamp; // arrival time — determines FIFO priority

    // Convenience: how much is still open
    Quantity open_quantity() const {
        return quantity - filled;
    }

    bool is_terminal() const {
        return status == OrderStatus::Filled ||
               status == OrderStatus::Cancelled;
    }
};

} // namespace orderbook