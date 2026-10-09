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
        o.user_id   = cmd.user_id;
        o.side      = cmd.side;
        o.type      = OrderType::Limit;
        o.status    = OrderStatus::Accepted;
        o.tif       = cmd.tif;
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
        o.user_id   = cmd.user_id;
        o.side      = cmd.side;
        o.type      = OrderType::Market;
        o.status    = OrderStatus::Accepted;
        o.tif       = cmd.tif;
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
    case CommandKind::ModifyOrder: {
        // cmd.price = new_price, cmd.quantity = new_qty, cmd.timestamp = new_ts
        auto fills = book.modify(cmd.id, cmd.price, cmd.quantity, cmd.timestamp);
        // For the differential harness we treat modify as accepted if the id was
        // known (book.modify returns empty fills on unknown id but doesn't return
        // a bool). We detect unknown by checking order_index_ absence... but
        // since both books run the same command in lockstep, comparing results is
        // sufficient. Return accepted=true unconditionally and compare fills.
        return CommandResult{true, std::move(fills)};
    }
    }
    return CommandResult{false, {}};
}

} // namespace orderbook::test
