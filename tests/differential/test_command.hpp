#pragma once

#include "orderbook/types.hpp"
#include <ostream>

namespace orderbook::test {

enum class CommandKind : uint8_t {
    LimitOrder,
    MarketOrder,
    CancelOrder
};

struct TestCommand {
    CommandKind kind{CommandKind::LimitOrder};
    OrderId     id{0};
    Side        side{Side::Buy};
    Price       price{0};
    Quantity    quantity{0};
    Timestamp   timestamp{0};

    static TestCommand limit(OrderId id, Side side, Price price, Quantity qty, Timestamp ts = 0) {
        return TestCommand{CommandKind::LimitOrder, id, side, price, qty, ts};
    }

    static TestCommand market(OrderId id, Side side, Quantity qty, Timestamp ts = 0) {
        return TestCommand{CommandKind::MarketOrder, id, side, 0, qty, ts};
    }

    static TestCommand cancel(OrderId id) {
        return TestCommand{CommandKind::CancelOrder, id, Side::Buy, 0, 0, 0};
    }

    bool operator==(const TestCommand& other) const = default;
};

inline std::ostream& operator<<(std::ostream& os, const TestCommand& cmd) {
    switch (cmd.kind) {
    case CommandKind::LimitOrder:
        os << "LIMIT [id=" << cmd.id << " " << (cmd.side == Side::Buy ? "BUY" : "SELL")
           << " qty=" << cmd.quantity << " @" << cmd.price << " ts=" << cmd.timestamp << "]";
        break;
    case CommandKind::MarketOrder:
        os << "MARKET [id=" << cmd.id << " " << (cmd.side == Side::Buy ? "BUY" : "SELL")
           << " qty=" << cmd.quantity << " ts=" << cmd.timestamp << "]";
        break;
    case CommandKind::CancelOrder:
        os << "CANCEL [id=" << cmd.id << "]";
        break;
    }
    return os;
}

} // namespace orderbook::test

namespace ob = orderbook;