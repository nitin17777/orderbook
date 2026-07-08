#pragma once

#include "orderbook/order.hpp"
#include "orderbook/fill.hpp"

#include <vector>
#include <deque>
#include <unordered_map>
#include <optional>
#include <cassert>
#include <limits>

namespace orderbook {

inline constexpr Price MIN_PRICE = 1;
inline constexpr Price MAX_PRICE = 10'000;
inline constexpr std::size_t NUM_LEVELS =
    static_cast<std::size_t>(MAX_PRICE - MIN_PRICE + 1);

// ── Order Pool ────────────────────────────────────────────────────────────────

class OrderPool {
public:
    void insert(Order order) {
        pool_[order.id] = std::move(order);
    }

    Order* get(OrderId id) {
        auto it = pool_.find(id);
        return it == pool_.end() ? nullptr : &it->second;
    }

    const Order* get(OrderId id) const {
        auto it = pool_.find(id);
        return it == pool_.end() ? nullptr : &it->second;
    }

    void erase(OrderId id) { pool_.erase(id); }
    void clear()           { pool_.clear(); }   // ← added

    std::size_t size() const { return pool_.size(); }

private:
    std::unordered_map<OrderId, Order> pool_;
};

// ── Price Level Array ─────────────────────────────────────────────────────────

class PriceLevelArray {
public:
    PriceLevelArray() : levels_(NUM_LEVELS) {}

    void push(Price price, OrderId id) {
        levels_[index(price)].push_back(id);
    }

    OrderId front(Price price) const {
        const auto& q = levels_[index(price)];
        return q.empty() ? INVALID_ORDER_ID : q.front();
    }

    void pop_front(Price price) {
        levels_[index(price)].pop_front();  // O(1) — deque
    }

    bool empty(Price price) const {
        return levels_[index(price)].empty();
    }

    // Clear all levels — each deque.clear() is O(n) elements,
    // but the deque objects themselves stay allocated in the vector.
    // This is the cheap reset — no heap alloc/free of the 10k slots.
    void clear() {                          // ← added
        for (auto& q : levels_) q.clear();
    }

private:
    std::vector<std::deque<OrderId>> levels_;

    static std::size_t index(Price price) {
        assert(price >= MIN_PRICE && price <= MAX_PRICE);
        return static_cast<std::size_t>(price - MIN_PRICE);
    }
};

// ── FastOrderBook ─────────────────────────────────────────────────────────────

class FastOrderBook {
public:
    std::vector<Fill> add(Order order);
    bool cancel(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    const Order* find(OrderId id) const { return pool_.get(id); }
    std::size_t  order_count()    const { return pool_.size(); }

    // Reset all state. The 10k deque slots stay allocated — only contents
    // are cleared. Construction cost is paid once; reset is cheap per iter.
    void reset() {                          // ← replaced old version
        bids_.clear();
        asks_.clear();
        pool_.clear();
        best_bid_price_ = MIN_PRICE - 1;
        best_ask_price_ = MAX_PRICE + 1;
    }

private:
    PriceLevelArray bids_;
    PriceLevelArray asks_;
    OrderPool       pool_;

    Price best_bid_price_ = MIN_PRICE - 1;
    Price best_ask_price_ = MAX_PRICE + 1;

    std::vector<Fill> match(Order& incoming);
    void rest(Order order);
    void update_best_bid_after_removal();
    void update_best_ask_after_removal();
};

} // namespace orderbook