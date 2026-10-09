#pragma once

#include "orderbook/types.hpp"
#include <ostream>

namespace orderbook::test {

enum class CommandKind : uint8_t {
    LimitOrder,
    MarketOrder,
    CancelOrder,
    ModifyOrder
};

struct TestCommand {
    CommandKind kind{CommandKind::LimitOrder};
    OrderId     id{0};
    UserId      user_id{INVALID_USER_ID};
    Side        side{Side::Buy};
    Price       price{0};
    Quantity    quantity{0};
    Timestamp   timestamp{0};
    TimeInForce tif{TimeInForce::GTC};

    static TestCommand limit(OrderId id, Side side, Price price, Quantity qty, Timestamp ts = 0, TimeInForce tif = TimeInForce::GTC, UserId user_id = INVALID_USER_ID) {
        return TestCommand{CommandKind::LimitOrder, id, user_id, side, price, qty, ts, tif};
    }

    static TestCommand ioc(OrderId id, Side side, Price price, Quantity qty, Timestamp ts = 0, UserId user_id = INVALID_USER_ID) {
        return TestCommand{CommandKind::LimitOrder, id, user_id, side, price, qty, ts, TimeInForce::IOC};
    }

    static TestCommand fok(OrderId id, Side side, Price price, Quantity qty, Timestamp ts = 0, UserId user_id = INVALID_USER_ID) {
        return TestCommand{CommandKind::LimitOrder, id, user_id, side, price, qty, ts, TimeInForce::FOK};
    }

    static TestCommand market(OrderId id, Side side, Quantity qty, Timestamp ts = 0, TimeInForce tif = TimeInForce::IOC, UserId user_id = INVALID_USER_ID) {
        return TestCommand{CommandKind::MarketOrder, id, user_id, side, 0, qty, ts, tif};
    }

    static TestCommand cancel(OrderId id, UserId user_id = INVALID_USER_ID) {
        return TestCommand{CommandKind::CancelOrder, id, user_id, Side::Buy, 0, 0, 0, TimeInForce::GTC};
    }

    // Modify: new_price stored in 'price', new_qty in 'quantity', new_ts in 'timestamp'
    static TestCommand modify(OrderId id, Price new_price, Quantity new_qty, Timestamp ts = 0, UserId user_id = INVALID_USER_ID) {
        return TestCommand{CommandKind::ModifyOrder, id, user_id, Side::Buy, new_price, new_qty, ts, TimeInForce::GTC};
    }

    bool operator==(const TestCommand& other) const = default;
};

inline std::ostream& operator<<(std::ostream& os, const TestCommand& cmd) {
    switch (cmd.kind) {
    case CommandKind::LimitOrder:
        os << "LIMIT [" << to_string(cmd.tif) << " id=" << cmd.id << " user=" << cmd.user_id << " " << (cmd.side == Side::Buy ? "BUY" : "SELL")
           << " qty=" << cmd.quantity << " @" << cmd.price << " ts=" << cmd.timestamp << "]";
        break;
    case CommandKind::MarketOrder:
        os << "MARKET [" << to_string(cmd.tif) << " id=" << cmd.id << " user=" << cmd.user_id << " " << (cmd.side == Side::Buy ? "BUY" : "SELL")
           << " qty=" << cmd.quantity << " ts=" << cmd.timestamp << "]";
        break;
    case CommandKind::CancelOrder:
        os << "CANCEL [id=" << cmd.id << " user=" << cmd.user_id << "]";
        break;
    case CommandKind::ModifyOrder:
        os << "MODIFY [id=" << cmd.id << " user=" << cmd.user_id << " new_price=" << cmd.price
           << " new_qty=" << cmd.quantity << " ts=" << cmd.timestamp << "]";
        break;
    }
    return os;
}

} // namespace orderbook::test

namespace ob = orderbook;