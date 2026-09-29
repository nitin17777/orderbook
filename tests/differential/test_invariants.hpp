#pragma once

#include "test_snapshot.hpp"
#include <unordered_set>
#include <string>
#include <optional>
#include <sstream>

namespace orderbook::test {

struct InvariantCheckResult {
    bool ok{true};
    std::string message{};

    static InvariantCheckResult pass() { return {true, ""}; }
    static InvariantCheckResult fail(const std::string& msg) { return {false, msg}; }
};

inline InvariantCheckResult verify_invariants(const BookSnapshot& snap) {
    // 1. Uncrossed Book: Best Bid < Best Ask
    if (!snap.bids.empty() && !snap.asks.empty()) {
        Price best_bid = snap.bids.front().price;
        Price best_ask = snap.asks.front().price;
        if (best_bid >= best_ask) {
            std::ostringstream ss;
            ss << "Crossed book violation: best_bid (" << best_bid
               << ") >= best_ask (" << best_ask << ")";
            return InvariantCheckResult::fail(ss.str());
        }
    }

    // 2. Strict Price Ordering: Bids descending, Asks ascending
    for (size_t i = 1; i < snap.bids.size(); ++i) {
        if (snap.bids[i].price >= snap.bids[i - 1].price) {
            std::ostringstream ss;
            ss << "Bid price ordering violation: level " << i << " (@" << snap.bids[i].price
               << ") >= level " << (i - 1) << " (@" << snap.bids[i - 1].price << ")";
            return InvariantCheckResult::fail(ss.str());
        }
    }

    for (size_t i = 1; i < snap.asks.size(); ++i) {
        if (snap.asks[i].price <= snap.asks[i - 1].price) {
            std::ostringstream ss;
            ss << "Ask price ordering violation: level " << i << " (@" << snap.asks[i].price
               << ") <= level " << (i - 1) << " (@" << snap.asks[i - 1].price << ")";
            return InvariantCheckResult::fail(ss.str());
        }
    }

    // 3. Positive Quantities, Non-empty Levels, and Order Uniqueness
    std::unordered_set<OrderId> seen_ids;

    auto check_levels = [&](const std::vector<LevelSnapshot>& levels, const char* side_name) -> std::optional<std::string> {
        for (const auto& lvl : levels) {
            if (lvl.orders.empty()) {
                return std::string("Empty level in snapshot on side ") + side_name + " @" + std::to_string(lvl.price);
            }
            if (lvl.total_quantity() == 0) {
                return std::string("Zero total quantity on level @" + std::to_string(lvl.price));
            }
            for (const auto& [id, qty] : lvl.orders) {
                if (qty == 0) {
                    return std::string("Zero quantity for order ID ") + std::to_string(id) + " on level @" + std::to_string(lvl.price);
                }
                if (!seen_ids.insert(id).second) {
                    return std::string("Duplicate Order ID ") + std::to_string(id) + " detected across book levels";
                }
            }
        }
        return std::nullopt;
    };

    if (auto err = check_levels(snap.bids, "BID")) return InvariantCheckResult::fail(*err);
    if (auto err = check_levels(snap.asks, "ASK")) return InvariantCheckResult::fail(*err);

    return InvariantCheckResult::pass();
}

} // namespace orderbook::test
