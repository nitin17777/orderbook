#include "orderbook/book.hpp"

namespace orderbook {

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

const Order* OrderBook::find(OrderId id) const {
    auto it = order_index_.find(id);
    if (it == order_index_.end()) return nullptr;

    const auto& loc = it->second;
    if (loc.side == Side::Buy) {
        auto level_it = bids_.find(loc.price);
        if (level_it == bids_.end()) return nullptr;
        for (const auto& o : level_it->second)
            if (o.id == id) return &o;
    } else {
        auto level_it = asks_.find(loc.price);
        if (level_it == asks_.end()) return nullptr;
        for (const auto& o : level_it->second)
            if (o.id == id) return &o;
    }
    return nullptr;
}

void OrderBook::rest(Order order) {
    order_index_[order.id] = { order.price, order.side };
    if (order.side == Side::Buy)
        bids_[order.price].push_back(order);
    else
        asks_[order.price].push_back(order);
}

void OrderBook::remove_from_index(OrderId id) {
    order_index_.erase(id);
}

// Core matching logic
std::vector<Fill> OrderBook::match(Order& incoming) {
    std::vector<Fill> fills;

    // Choose the opposite side levels to match against
    // Buy order matches against asks (ascending), sell against bids (descending)
    auto try_match = [&](auto& levels) {
        while (incoming.open_quantity() > 0 && !levels.empty()) {
            auto level_it = levels.begin(); // best price on opposite side
            Price level_price = level_it->first;

            // Price check — limit orders cannot match beyond their limit
            if (incoming.type == OrderType::Limit) {
                if (incoming.side == Side::Buy  && level_price > incoming.price) break;
                if (incoming.side == Side::Sell && level_price < incoming.price) break;
            }

            auto& queue = level_it->second;

            while (incoming.open_quantity() > 0 && !queue.empty()) {
                Order& maker = queue.front();
                Quantity trade_qty = std::min(incoming.open_quantity(),
                                              maker.open_quantity());

                // Record the fill
                fills.push_back({ incoming.id, maker.id, level_price, trade_qty });

                // Update quantities
                incoming.filled += trade_qty;
                maker.filled    += trade_qty;

                // Update statuses
                if (incoming.open_quantity() == 0)
                    incoming.status = OrderStatus::Filled;
                else
                    incoming.status = OrderStatus::PartiallyFilled;

                if (maker.open_quantity() == 0) {
                    maker.status = OrderStatus::Filled;
                    remove_from_index(maker.id);
                    queue.pop_front();
                }
            }

            // Clean up empty price level
            if (queue.empty())
                levels.erase(level_it);
        }
    };

    if (incoming.side == Side::Buy)
        try_match(asks_);
    else
        try_match(bids_);

    return fills;
}

std::vector<Fill> OrderBook::add(Order order) {
    // Market orders never rest — cancel remainder after matching
    auto fills = match(order);

    if (order.type == OrderType::Market) {
        // Whatever didn't fill is cancelled — market orders don't rest
        if (order.open_quantity() > 0)
            order.status = OrderStatus::Cancelled;
        return fills;
    }

    // Limit order: rest remainder in book if not fully filled
    if (order.open_quantity() > 0)
        rest(order);

    return fills;
}

bool OrderBook::cancel(OrderId id) {
    auto it = order_index_.find(id);
    if (it == order_index_.end()) return false; // not found — silent no-op

    const auto& loc = it->second;

    auto cancel_from = [&](auto& levels) -> bool {
        auto level_it = levels.find(loc.price);
        if (level_it == levels.end()) return false;

        auto& queue = level_it->second;
        for (auto q_it = queue.begin(); q_it != queue.end(); ++q_it) {
            if (q_it->id == id) {
                q_it->status = OrderStatus::Cancelled;
                queue.erase(q_it);
                if (queue.empty())
                    levels.erase(level_it);
                remove_from_index(id);
                return true;
            }
        }
        return false;
    };

    if (loc.side == Side::Buy)
        return cancel_from(bids_);
    else
        return cancel_from(asks_);
}

} // namespace orderbook