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
    PriceLevelArray() : levels_(NUM_LEVELS), qty_(NUM_LEVELS, 0) {}

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

    void erase(Price price, OrderId id) {
        auto& q = levels_[index(price)];
        for (auto it = q.begin(); it != q.end(); ++it) {
            if (*it == id) {
                q.erase(it);
                break;
            }
        }
    }

    bool empty(Price price) const {
        return levels_[index(price)].empty();
    }

    const std::deque<OrderId>& queue(Price price) const {
        return levels_[index(price)];
    }

    // ── Per-level aggregate quantity ──────────────────────────────────────────
    // Maintained incrementally by FastOrderBook (not by PriceLevelArray itself).
    // PriceLevelArray exposes mutable accessors so FastOrderBook can adjust them.
    Quantity& qty(Price price)        { return qty_[index(price)]; }
    Quantity  qty(Price price) const  { return qty_[index(price)]; }

    // Clear all levels — each deque.clear() is O(n) elements,
    // but the deque objects themselves stay allocated in the vector.
    // This is the cheap reset — no heap alloc/free of the 10k slots.
    void clear() {
        for (auto& q : levels_) q.clear();
        std::fill(qty_.begin(), qty_.end(), Quantity{0});
    }

private:
    std::vector<std::deque<OrderId>> levels_;
    std::vector<Quantity>            qty_;     // per-level aggregate resting qty

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
    std::vector<Fill> modify(OrderId id, Price new_price, Quantity new_qty, Timestamp ts = 0);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    const Order* find(OrderId id) const { return pool_.get(id); }
    std::size_t  order_count()    const { return pool_.size(); }

    const PriceLevelArray& bids() const { return bids_; }
    const PriceLevelArray& asks() const { return asks_; }
    const OrderPool&       pool() const { return pool_; }

    // ── Per-level aggregate quantity (L2 depth support) ───────────────────────
    // Returns the total resting quantity at the given price level.
    // The aggregate is maintained incrementally:
    //   • Incremented when an order rests (rest()).
    //   • Decremented by fill quantity during matching.
    //   • Decremented by remaining_qty at cancel time (lazy-cancel policy:
    //     tombstones left in the deque are NOT counted — the aggregate is
    //     adjusted immediately when cancel() is called, not when the stale
    //     ID is later skipped during a match walk).
    Quantity bid_level_qty(Price price) const { return bids_.qty(price); }
    Quantity ask_level_qty(Price price) const { return asks_.qty(price); }

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
    bool can_fully_fill(const Order& incoming) const;
    void rest(Order order);
    void update_best_bid_after_removal();
    void update_best_ask_after_removal();
};

} // namespace orderbook