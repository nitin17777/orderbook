#pragma once

#include "orderbook/book.hpp"
#include "orderbook/fast_book.hpp"
#include "orderbook/types.hpp"

#include <vector>
#include <utility>
#include <string>
#include <ostream>

namespace orderbook::test {

struct LevelSnapshot {
    Price price{0};
    std::vector<std::pair<OrderId, Quantity>> orders{}; // [OrderId, open_qty] in FIFO order

    Quantity total_quantity() const {
        Quantity sum = 0;
        for (const auto& [id, qty] : orders) sum += qty;
        return sum;
    }

    bool operator==(const LevelSnapshot& other) const = default;
};

struct BookSnapshot {
    std::vector<LevelSnapshot> bids{}; // Descending by price
    std::vector<LevelSnapshot> asks{}; // Ascending by price

    bool operator==(const BookSnapshot& other) const = default;

    std::string to_string() const;
    static std::string diff_string(const BookSnapshot& expected, const BookSnapshot& actual);
};

std::ostream& operator<<(std::ostream& os, const LevelSnapshot& level);
std::ostream& operator<<(std::ostream& os, const BookSnapshot& snapshot);

// Extract observable canonical snapshots from either implementation
BookSnapshot snapshot(const OrderBook& book);
BookSnapshot snapshot(const FastOrderBook& book);

} // namespace orderbook::test
