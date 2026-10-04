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
    Quantity open_qty = order.open_quantity();

    pool_.insert(std::move(order));

    if (s == Side::Buy) {
        bids_.push(p, id);
        bids_.qty(p) += open_qty;     // increment aggregate
        if (p > best_bid_price_) best_bid_price_ = p;
    } else {
        asks_.push(p, id);
        asks_.qty(p) += open_qty;     // increment aggregate
        if (p < best_ask_price_) best_ask_price_ = p;
    }
}

// Walk down from best_bid_price_ until we find a level with at least one
// live (non-cancelled, non-filled) order. During lazy deletion the deque
// still holds stale IDs, so we must peek into the pool to confirm.
void FastOrderBook::update_best_bid_after_removal() {
    while (best_bid_price_ >= MIN_PRICE) {
        // Drain any stale (lazily-cancelled) ids at this level
        while (!bids_.empty(best_bid_price_)) {
            OrderId front_id = bids_.front(best_bid_price_);
            const Order* o   = pool_.get(front_id);
            if (o && !o->is_terminal()) break; // found a real order
            bids_.pop_front(best_bid_price_);   // stale — remove eagerly
        }
        if (!bids_.empty(best_bid_price_)) break; // level has live orders
        --best_bid_price_;
    }
}

// Walk up from best_ask_price_ until we find a level with at least one
// live order (same logic as above).
void FastOrderBook::update_best_ask_after_removal() {
    while (best_ask_price_ <= MAX_PRICE) {
        while (!asks_.empty(best_ask_price_)) {
            OrderId front_id = asks_.front(best_ask_price_);
            const Order* o   = pool_.get(front_id);
            if (o && !o->is_terminal()) break;
            asks_.pop_front(best_ask_price_);
        }
        if (!asks_.empty(best_ask_price_)) break;
        ++best_ask_price_;
    }
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

                // Decrement aggregate on the maker (ask) side.
                // Note: we only adjust for actual matched quantity, NOT for lazy
                // tombstones skipped above — those were already subtracted at
                // cancel() time (lazy-cancel policy).
                asks_.qty(level_price) -= trade_qty;

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

                // Decrement aggregate on the maker (bid) side.
                bids_.qty(level_price) -= trade_qty;

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

    // Save side before erasing — pool_.erase() invalidates the pointer.
    const Side side = o->side;
    const Price price = o->price;
    const Quantity remaining = o->open_quantity();

    // Lazy-cancel policy: subtract the remaining quantity from the aggregate
    // immediately at cancel time.  The tombstone ID stays in the deque and
    // will be skipped during future match walks, but the aggregate is already
    // correct — we do NOT adjust it again when the stale ID is skipped.
    if (side == Side::Buy)
        bids_.qty(price) -= remaining;
    else
        asks_.qty(price) -= remaining;

    // Lazy deletion — mark cancelled in pool.
    o->status = OrderStatus::Cancelled;
    pool_.erase(id); // o is dangling after this line — do not use

    // Update best price tracking if needed.
    if (side == Side::Buy)
        update_best_bid_after_removal();
    else
        update_best_ask_after_removal();

    return true;
}

} // namespace orderbook