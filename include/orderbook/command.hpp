#pragma once

#include "orderbook/order.hpp"

namespace orderbook {

enum class CommandType : uint8_t {
    AddOrder    = 1,
    CancelOrder = 2
};

// A Command is the unit of the event log.
// Every state change to the book is represented as a Command first,
// then applied. This means the log IS the source of truth.
struct Command {
    CommandType type;
    union {
        Order    order;   // used when type == AddOrder
        OrderId  cancel_id; // used when type == CancelOrder
    };

    // Named constructors — cleaner than direct construction
    static Command add(Order o) {
        Command c;
        c.type  = CommandType::AddOrder;
        c.order = o;
        return c;
    }

    static Command cancel(OrderId id) {
        Command c;
        c.type      = CommandType::CancelOrder;
        c.cancel_id = id;
        return c;
    }
};
}