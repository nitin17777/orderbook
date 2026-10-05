#pragma once

#include "orderbook/order.hpp"

namespace orderbook {

enum class CommandType : uint8_t {
    AddOrder    = 1,
    CancelOrder = 2,
    ModifyOrder = 3
};

struct ModifyPayload {
    OrderId   id;
    Price     new_price;
    Quantity  new_quantity;
    Timestamp timestamp;
};

// A Command is the unit of the event log.
// Every state change to the book is represented as a Command first,
// then applied. This means the log IS the source of truth.
struct Command {
    CommandType type;
    union {
        Order         order;     // used when type == AddOrder
        OrderId       cancel_id; // used when type == CancelOrder
        ModifyPayload mod;      // used when type == ModifyOrder
    };

    Command() : type(CommandType::AddOrder), order{} {}

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

    static Command modify(OrderId id, Price new_price, Quantity new_qty, Timestamp ts = 0) {
        Command c;
        c.type             = CommandType::ModifyOrder;
        c.mod.id           = id;
        c.mod.new_price    = new_price;
        c.mod.new_quantity = new_qty;
        c.mod.timestamp    = ts;
        return c;
    }
};

static_assert(std::is_trivially_copyable_v<Command>, "Command must be trivially copyable for binary logging");

} // namespace orderbook