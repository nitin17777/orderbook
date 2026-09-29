#pragma once

#include "test_command.hpp"
#include "test_result.hpp"
#include "orderbook/order.hpp"

namespace orderbook::test {

template <typename BookType>
CommandResult execute_command(BookType& book, const TestCommand& cmd) {
    switch (cmd.kind) {
    case CommandKind::LimitOrder: {
        Order o{};
        o.id        = cmd.id;
        o.side      = cmd.side;
        o.type      = OrderType::Limit;
        o.status    = OrderStatus::Accepted;
        o.price     = cmd.price;
        o.quantity  = cmd.quantity;
        o.filled    = 0;
        o.timestamp = cmd.timestamp;
        auto fills = book.add(o);
        return CommandResult{true, std::move(fills)};
    }
    case CommandKind::MarketOrder: {
        Order o{};
        o.id        = cmd.id;
        o.side      = cmd.side;
        o.type      = OrderType::Market;
        o.status    = OrderStatus::Accepted;
        o.price     = 0;
        o.quantity  = cmd.quantity;
        o.filled    = 0;
        o.timestamp = cmd.timestamp;
        auto fills = book.add(o);
        return CommandResult{true, std::move(fills)};
    }
    case CommandKind::CancelOrder: {
        bool ok = book.cancel(cmd.id);
        return CommandResult{ok, {}};
    }
    }
    return CommandResult{false, {}};
}

} // namespace orderbook::test
