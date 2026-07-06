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

private:
    BidLevels bids_;
    AskLevels asks_;

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
};

} // namespace orderbook