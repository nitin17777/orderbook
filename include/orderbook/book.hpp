#pragma once

#include "orderbook/order.hpp"
#include "orderbook/fill.hpp"

#include <map>
#include <deque>
#include <unordered_map>
#include <vector>
#include <optional>
#include <functional>

namespace orderbook {

// Bids: highest price first  → std::greater
// Asks: lowest price first   → std::less (default)
using BidLevels = std::map<Price, std::deque<Order>, std::greater<Price>>;
using AskLevels = std::map<Price, std::deque<Order>>;

class OrderBook {
public:
    // Submit a new order. Returns all fills generated (may be empty).
    std::vector<Fill> add(Order order);

    // Cancel a resting order by id. Returns true if found and cancelled.
    bool cancel(OrderId id);

    // Accessors — for testing and display
    const BidLevels& bids() const { return bids_; }
    const AskLevels& asks() const { return asks_; }

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    // Returns nullptr if order not found or already terminal
    const Order* find(OrderId id) const;

    std::size_t order_count() const { return order_index_.size(); }

    // ── Per-level aggregate quantity (L2 depth support) ───────────────────────
    // Returns the total resting quantity at the given price level, or 0 if the
    // level does not exist.  Maintained incrementally: decremented on fill and
    // on cancel (lazy-cancel policy — adjusted at cancel time, not at tombstone
    // skip, because OrderBook uses eager deletion so there are no tombstones).
    Quantity bid_level_qty(Price price) const {
        auto it = bid_qty_.find(price);
        return (it != bid_qty_.end()) ? it->second : 0;
    }
    Quantity ask_level_qty(Price price) const {
        auto it = ask_qty_.find(price);
        return (it != ask_qty_.end()) ? it->second : 0;
    }

    // Full aggregated depth maps (price → total qty).  Bids and asks share the
    // same key type (Price); callers iterate bids in reverse for descending order.
    const std::map<Price, Quantity>& bid_quantities() const { return bid_qty_; }
    const std::map<Price, Quantity>& ask_quantities() const { return ask_qty_; }

private:
    BidLevels bids_;
    AskLevels asks_;

    // Per-level aggregate resting quantity (L2 depth aggregates).
    // Maintained incrementally alongside bids_/asks_.
    // Uses std::map<Price,Quantity> (ascending key) for both sides.
    // Invariant: qty > 0 iff the level exists in bids_/asks_.
    std::map<Price, Quantity> bid_qty_;
    std::map<Price, Quantity> ask_qty_;

    // id → pointer into the deque for O(1) cancel lookup
    // We store the price + side so we can find the right level fast
    struct OrderLocation {
        Price price;
        Side  side;
    };
    std::unordered_map<OrderId, OrderLocation> order_index_;

    // Internal matching — called by add()
    std::vector<Fill> match(Order& incoming);

    // Place a resting order into the book after matching
    void rest(Order order);

    // Remove a fully filled or cancelled order from the index
    void remove_from_index(OrderId id);

    // Adjust per-level aggregate: subtract `qty` from the given side/price.
    // Removes the entry when it reaches zero.
    void subtract_level_qty(Side side, Price price, Quantity qty);
};

} // namespace orderbook