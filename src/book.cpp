#include "orderbook/book.hpp"
#include <algorithm>

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
    if (order.side == Side::Buy) {
        bids_[order.price].push_back(order);
        bid_qty_[order.price] += order.open_quantity();
    } else {
        asks_[order.price].push_back(order);
        ask_qty_[order.price] += order.open_quantity();
    }
}

void OrderBook::remove_from_index(OrderId id) {
    order_index_.erase(id);
}

void OrderBook::subtract_level_qty(Side side, Price price, Quantity qty) {
    auto& qtys = (side == Side::Buy) ? bid_qty_ : ask_qty_;
    auto it = qtys.find(price);
    if (it == qtys.end()) return;
    if (it->second <= qty)
        qtys.erase(it);
    else
        it->second -= qty;
}

std::vector<Fill> OrderBook::match(Order& incoming) {
    std::vector<Fill> fills;

    auto try_match = [&](auto& levels) {
        while (incoming.status != OrderStatus::Cancelled && incoming.open_quantity() > 0 && !levels.empty()) {
            auto level_it = levels.begin();
            Price level_price = level_it->first;

            if (incoming.type == OrderType::Limit) {
                if (incoming.side == Side::Buy  && level_price > incoming.price) break;
                if (incoming.side == Side::Sell && level_price < incoming.price) break;
            }

            auto& queue = level_it->second;

            while (incoming.status != OrderStatus::Cancelled && incoming.open_quantity() > 0 && !queue.empty()) {
                Order& maker = queue.front();

                // Self-Trade Prevention (STP): Cancel Newest / Cancel Taker
                // When taker encounters its own resting maker, cancel the remaining taker quantity
                // and preserve the resting maker intact.
                if (incoming.user_id != INVALID_USER_ID && maker.user_id == incoming.user_id) {
                    incoming.status = OrderStatus::Cancelled;
                    break;
                }

                Quantity trade_qty = std::min(incoming.open_quantity(), maker.open_quantity());

                fills.push_back({ incoming.id, maker.id, level_price, trade_qty });

                incoming.filled += trade_qty;
                maker.filled    += trade_qty;

                Side maker_side = (incoming.side == Side::Buy) ? Side::Sell : Side::Buy;
                subtract_level_qty(maker_side, level_price, trade_qty);

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

bool OrderBook::can_fully_fill(const Order& incoming) const {
    Quantity needed = incoming.quantity;
    if (needed == 0) return true;

    if (incoming.side == Side::Buy) {
        for (const auto& [price, queue] : asks_) {
            if (incoming.type == OrderType::Limit && price > incoming.price) break;
            for (const auto& maker : queue) {
                // STP: Cannot match against own orders
                if (incoming.user_id != INVALID_USER_ID && maker.user_id == incoming.user_id) {
                    return false;
                }
                Quantity avail = maker.open_quantity();
                if (avail >= needed) return true;
                needed -= avail;
            }
        }
    } else {
        for (const auto& [price, queue] : bids_) {
            if (incoming.type == OrderType::Limit && price < incoming.price) break;
            for (const auto& maker : queue) {
                // STP: Cannot match against own orders
                if (incoming.user_id != INVALID_USER_ID && maker.user_id == incoming.user_id) {
                    return false;
                }
                Quantity avail = maker.open_quantity();
                if (avail >= needed) return true;
                needed -= avail;
            }
        }
    }
    return false;
}

std::vector<Fill> OrderBook::add(Order order) {
    if (order.tif == TimeInForce::FOK) {
        if (!can_fully_fill(order)) {
            order.status = OrderStatus::Cancelled;
            return {};
        }
    }

    auto fills = match(order);

    if (order.type == OrderType::Market || order.tif == TimeInForce::IOC || order.tif == TimeInForce::FOK) {
        if (order.open_quantity() > 0)
            order.status = OrderStatus::Cancelled;
        return fills;
    }

    // Limit GTC order: rest remainder only if not cancelled by STP
    if (order.status != OrderStatus::Cancelled && order.open_quantity() > 0)
        rest(order);

    return fills;
}

bool OrderBook::cancel(OrderId id) {
    auto it = order_index_.find(id);
    if (it == order_index_.end()) return false;

    const auto& loc = it->second;

    auto cancel_from = [&](auto& levels, Side side) -> bool {
        auto level_it = levels.find(loc.price);
        if (level_it == levels.end()) return false;

        auto& queue = level_it->second;
        for (auto q_it = queue.begin(); q_it != queue.end(); ++q_it) {
            if (q_it->id == id) {
                subtract_level_qty(side, loc.price, q_it->open_quantity());
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
        return cancel_from(bids_, Side::Buy);
    else
        return cancel_from(asks_, Side::Sell);
}

// Priority rules:
//   Reducing quantity at same price     => retain queue position (in-place edit)
//   Increasing quantity at same price   => lose queue position (cancel + re-add)
//   Changing price (any direction)      => lose queue position (cancel + re-add)
//
// The cancel-replace path goes through add() so a price-crossing modify may
// immediately fill against resting orders on the opposite side.
std::vector<Fill> OrderBook::modify(OrderId id, Price new_price, Quantity new_qty, Timestamp ts) {
    auto it = order_index_.find(id);
    if (it == order_index_.end()) return {};

    const auto loc = it->second;

    // Must use a generic lambda to handle BidLevels (std::greater) and
    // AskLevels (std::less) separately; a ternary auto& fails because the
    // two map types have different comparators and are not the same type.
    auto do_modify = [&](auto& levels) -> std::vector<Fill> {
        auto level_it = levels.find(loc.price);
        if (level_it == levels.end()) return {};

        auto& queue = level_it->second;
        for (auto q_it = queue.begin(); q_it != queue.end(); ++q_it) {
            if (q_it->id != id) continue;

            const Quantity old_open = q_it->open_quantity();

            // Priority-retaining: same price, quantity can only shrink
            if (new_price == loc.price && new_qty <= old_open) {
                const Quantity reduce_by = old_open - new_qty;
                q_it->quantity -= reduce_by;
                subtract_level_qty(loc.side, loc.price, reduce_by);
                return {};
            }

            // Priority-losing: cancel the resting order, re-add at back of queue
            Order updated = *q_it;
            subtract_level_qty(loc.side, loc.price, old_open);
            queue.erase(q_it);
            if (queue.empty())
                levels.erase(level_it);
            remove_from_index(id);

            updated.price     = new_price;
            updated.quantity  = new_qty;
            updated.filled    = 0;
            updated.timestamp = ts;
            updated.status    = OrderStatus::Accepted;

            return add(updated);
        }
        return {};
    };

    if (loc.side == Side::Buy)
        return do_modify(bids_);
    else
        return do_modify(asks_);
}

} // namespace orderbook
