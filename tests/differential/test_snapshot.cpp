#include "test_snapshot.hpp"

#include <sstream>
#include <iomanip>

namespace orderbook::test {

std::string BookSnapshot::to_string() const {
    std::ostringstream ss;
    ss << *this;
    return ss.str();
}

std::string BookSnapshot::diff_string(const BookSnapshot& expected, const BookSnapshot& actual) {
    std::ostringstream ss;
    ss << "=== EXPECTED (Naive) ===\n" << expected
       << "\n=== ACTUAL (Fast) ===\n" << actual;
    return ss.str();
}

std::ostream& operator<<(std::ostream& os, const LevelSnapshot& level) {
    os << "Level @" << level.price << " [total=" << level.total_quantity() << "]: (";
    for (size_t i = 0; i < level.orders.size(); ++i) {
        if (i > 0) os << ", ";
        os << "id=" << level.orders[i].first << ":qty=" << level.orders[i].second;
    }
    os << ")";
    return os;
}

std::ostream& operator<<(std::ostream& os, const BookSnapshot& snapshot) {
    os << "BookSnapshot:\n  BIDS (" << snapshot.bids.size() << " levels):\n";
    for (const auto& level : snapshot.bids) {
        os << "    " << level << "\n";
    }
    os << "  ASKS (" << snapshot.asks.size() << " levels):\n";
    for (const auto& level : snapshot.asks) {
        os << "    " << level << "\n";
    }
    return os;
}

BookSnapshot snapshot(const OrderBook& book) {
    BookSnapshot result;

    for (const auto& [price, deque] : book.bids()) {
        LevelSnapshot level;
        level.price = price;
        for (const auto& order : deque) {
            if (order.open_quantity() > 0) {
                level.orders.emplace_back(order.id, order.open_quantity());
            }
        }
        if (!level.orders.empty()) {
            result.bids.push_back(std::move(level));
        }
    }

    for (const auto& [price, deque] : book.asks()) {
        LevelSnapshot level;
        level.price = price;
        for (const auto& order : deque) {
            if (order.open_quantity() > 0) {
                level.orders.emplace_back(order.id, order.open_quantity());
            }
        }
        if (!level.orders.empty()) {
            result.asks.push_back(std::move(level));
        }
    }

    return result;
}

BookSnapshot snapshot(const FastOrderBook& book) {
    BookSnapshot result;

    // Bids descending: MIN_PRICE to MAX_PRICE
    for (Price p = MAX_PRICE; p >= MIN_PRICE; --p) {
        const auto& q = book.bids().queue(p);
        if (q.empty()) continue;

        LevelSnapshot level;
        level.price = p;
        for (OrderId id : q) {
            const Order* o = book.pool().get(id);
            if (o && !o->is_terminal() && o->open_quantity() > 0) {
                level.orders.emplace_back(id, o->open_quantity());
            }
        }
        if (!level.orders.empty()) {
            result.bids.push_back(std::move(level));
        }
    }

    // Asks ascending: MIN_PRICE to MAX_PRICE
    for (Price p = MIN_PRICE; p <= MAX_PRICE; ++p) {
        const auto& q = book.asks().queue(p);
        if (q.empty()) continue;

        LevelSnapshot level;
        level.price = p;
        for (OrderId id : q) {
            const Order* o = book.pool().get(id);
            if (o && !o->is_terminal() && o->open_quantity() > 0) {
                level.orders.emplace_back(id, o->open_quantity());
            }
        }
        if (!level.orders.empty()) {
            result.asks.push_back(std::move(level));
        }
    }

    return result;
}

} // namespace orderbook::test
