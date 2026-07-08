#include "orderbook/fast_book.hpp"

namespace orderbook {

std::optional<Price> FastOrderBook::best_bid() const {
    if (best_bid_price_ < MIN_PRICE) return std::nullopt;
    return best_bid_price_;
}

std::optional<Price> FastOrderBook::best_ask() const {
    if (best_ask_price_ > MAX_PRICE) return std::nullopt;
    return best_ask_price_;
}

void FastOrderBook::rest(Order order) {
    Price p = order.price;
    Side  s = order.side;
    OrderId id = order.id;

    pool_.insert(std::move(order));

    if (s == Side::Buy) {
        bids_.push(p, id);
        if (p > best_bid_price_) best_bid_price_ = p;
    } else {
        asks_.push(p, id);
        if (p < best_ask_price_) best_ask_price_ = p;
    }
}

// Walk down from best_bid_price_ until we find a non-empty level
void FastOrderBook::update_best_bid_after_removal() {
    while (best_bid_price_ >= MIN_PRICE && bids_.empty(best_bid_price_))
        --best_bid_price_;
}

// Walk up from best_ask_price_ until we find a non-empty level
void FastOrderBook::update_best_ask_after_removal() {
    while (best_ask_price_ <= MAX_PRICE && asks_.empty(best_ask_price_))
        ++best_ask_price_;
}

std::vector<Fill> FastOrderBook::match(Order& incoming) {
    std::vector<Fill> fills;

    if (incoming.side == Side::Buy) {
        // Match against asks — walk up from best ask
        while (incoming.open_quantity() > 0 &&
               best_ask_price_ <= MAX_PRICE) {

            Price level_price = best_ask_price_;

            // Limit order price check
            if (incoming.type == OrderType::Limit &&
                level_price > incoming.price) break;

            // Drain this level
            while (incoming.open_quantity() > 0 &&
                   !asks_.empty(level_price)) {

                OrderId maker_id = asks_.front(level_price);

                // Lazy deletion — skip cancelled orders
                Order* maker = pool_.get(maker_id);
                if (!maker || maker->is_terminal()) {
                    asks_.pop_front(level_price);
                    continue;
                }

                Quantity trade_qty = std::min(incoming.open_quantity(),
                                              maker->open_quantity());

                fills.push_back({ incoming.id, maker_id, level_price, trade_qty });

                incoming.filled += trade_qty;
                maker->filled   += trade_qty;

                if (incoming.open_quantity() == 0)
                    incoming.status = OrderStatus::Filled;
                else
                    incoming.status = OrderStatus::PartiallyFilled;

                if (maker->open_quantity() == 0) {
                    maker->status = OrderStatus::Filled;
                    pool_.erase(maker_id);
                    asks_.pop_front(level_price);
                }
            }

            if (asks_.empty(level_price)) {
                update_best_ask_after_removal();
            }
        }
    } else {
        // Match against bids — walk down from best bid
        while (incoming.open_quantity() > 0 &&
               best_bid_price_ >= MIN_PRICE) {

            Price level_price = best_bid_price_;

            if (incoming.type == OrderType::Limit &&
                level_price < incoming.price) break;

            while (incoming.open_quantity() > 0 &&
                   !bids_.empty(level_price)) {

                OrderId maker_id = bids_.front(level_price);

                Order* maker = pool_.get(maker_id);
                if (!maker || maker->is_terminal()) {
                    bids_.pop_front(level_price);
                    continue;
                }

                Quantity trade_qty = std::min(incoming.open_quantity(),
                                              maker->open_quantity());

                fills.push_back({ incoming.id, maker_id, level_price, trade_qty });

                incoming.filled += trade_qty;
                maker->filled   += trade_qty;

                if (incoming.open_quantity() == 0)
                    incoming.status = OrderStatus::Filled;
                else
                    incoming.status = OrderStatus::PartiallyFilled;

                if (maker->open_quantity() == 0) {
                    maker->status = OrderStatus::Filled;
                    pool_.erase(maker_id);
                    bids_.pop_front(level_price);
                }
            }

            if (bids_.empty(level_price)) {
                update_best_bid_after_removal();
            }
        }
    }

    return fills;
}

std::vector<Fill> FastOrderBook::add(Order order) {
    auto fills = match(order);

    if (order.type == OrderType::Market) {
        // Market orders never rest
        return fills;
    }

    if (order.open_quantity() > 0)
        rest(order);

    return fills;
}

bool FastOrderBook::cancel(OrderId id) {
    Order* o = pool_.get(id);
    if (!o || o->is_terminal()) return false;

    // Lazy deletion — mark cancelled in pool.
    // The queue entry is cleaned up next time matching walks past it.
    o->status = OrderStatus::Cancelled;
    pool_.erase(id);

    // Update best price tracking if needed
    if (o->side == Side::Buy)
        update_best_bid_after_removal();
    else
        update_best_ask_after_removal();

    return true;
}

} // namespace orderbook