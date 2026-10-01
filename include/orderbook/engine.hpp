#pragma once

#include "orderbook/book.hpp"
#include "orderbook/event_log.hpp"
#include "orderbook/types.hpp"

#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <utility>

namespace orderbook {

// Result of submitting an order to the Engine
struct OrderResult {
    bool              accepted{false};
    RejectReason      reason{RejectReason::None};
    std::vector<Fill> fills{};

    explicit operator bool() const { return accepted; }
};

// Result of cancelling an order in the Engine
struct CancelResult {
    bool         accepted{false};
    RejectReason reason{RejectReason::None};

    explicit operator bool() const { return accepted; }
};

// Engine wraps OrderBook with an EventLog, identity tracking, and input validation.
// Every command is validated prior to state mutation.
// Valid commands are logged before being applied (event-sourcing principle).
// The book can be reconstructed from the log at any time via replay().
class Engine {
public:
    explicit Engine(const std::string& log_path)
        : log_(log_path)
    {}

    // Submit a new order. Validates input before any book mutation or logging.
    OrderResult add(Order order) {
        // 1. Validation phase (Zero book mutation / Zero log writes on rejection)
        if (order.quantity == 0) {
            return OrderResult{false, RejectReason::InvalidQuantity, {}};
        }

        if (order.type == OrderType::Limit && order.price <= 0) {
            return OrderResult{false, RejectReason::InvalidPrice, {}};
        }

        // Validate duplicate internal OrderId if explicitly supplied by caller
        if (order.id != INVALID_ORDER_ID) {
            if (book_.find(order.id) != nullptr) {
                return OrderResult{false, RejectReason::DuplicateOrderId, {}};
            }
        }

        // Validate duplicate ClientOrderId for this user
        if (order.user_id != INVALID_USER_ID && order.client_order_id != INVALID_CLIENT_ORDER_ID) {
            UserClientKey key{order.user_id, order.client_order_id};
            if (client_to_order_id_.find(key) != client_to_order_id_.end()) {
                return OrderResult{false, RejectReason::DuplicateClientOrderId, {}};
            }
        }

        // 2. Order ID Assignment
        if (order.id == INVALID_ORDER_ID) {
            order.id = next_order_id_++;
        } else {
            next_order_id_ = std::max(next_order_id_, order.id + 1);
        }

        // 3. State Transition: Log first (source of truth), then mutate book
        log_.append(Command::add(order));
        auto fills = book_.add(order);

        // 4. Indexing: Clean up fully-filled makers from client mapping
        for (const auto& f : fills) {
            if (book_.find(f.maker_id) == nullptr) {
                cleanup_client_mapping(f.maker_id);
            }
        }

        // If the incoming order rested in the book, index its client ID mapping
        if (book_.find(order.id) != nullptr) {
            if (order.user_id != INVALID_USER_ID && order.client_order_id != INVALID_CLIENT_ORDER_ID) {
                UserClientKey key{order.user_id, order.client_order_id};
                client_to_order_id_[key] = order.id;
                order_to_client_key_[order.id] = key;
            }
        }

        return OrderResult{true, RejectReason::None, std::move(fills)};
    }

    // Cancel a resting order by internal OrderId. Validates existence and ownership.
    CancelResult cancel(OrderId id, UserId user_id = INVALID_USER_ID) {
        const Order* existing = book_.find(id);
        if (existing == nullptr) {
            return CancelResult{false, RejectReason::UnknownOrder};
        }

        if (user_id != INVALID_USER_ID && existing->user_id != INVALID_USER_ID && existing->user_id != user_id) {
            return CancelResult{false, RejectReason::Unauthorized};
        }

        // Log before mutating book
        log_.append(Command::cancel(id));
        book_.cancel(id);
        cleanup_client_mapping(id);

        return CancelResult{true, RejectReason::None};
    }

    // Cancel an order by user_id and client_order_id
    CancelResult cancel_by_client_id(UserId user_id, ClientOrderId client_order_id) {
        if (user_id == INVALID_USER_ID || client_order_id == INVALID_CLIENT_ORDER_ID) {
            return CancelResult{false, RejectReason::UnknownOrder};
        }

        UserClientKey key{user_id, client_order_id};
        auto it = client_to_order_id_.find(key);
        if (it == client_to_order_id_.end()) {
            return CancelResult{false, RejectReason::UnknownOrder};
        }

        return cancel(it->second, user_id);
    }

    // Lookup internal OrderId from UserId and ClientOrderId
    OrderId lookup_client_order(UserId user_id, ClientOrderId client_order_id) const {
        UserClientKey key{user_id, client_order_id};
        auto it = client_to_order_id_.find(key);
        return it != client_to_order_id_.end() ? it->second : INVALID_ORDER_ID;
    }

    void flush() { log_.flush(); }

    const OrderBook& book()     const { return book_; }
    const std::string& log_path() const { return log_.path(); }

    // Replay a log file into a fresh OrderBook.
    // Returns the reconstructed book and the fill history.
    struct ReplayResult {
        OrderBook          book;
        std::vector<Fill>  fills;
    };

    static ReplayResult replay(const std::string& log_path) {
        auto commands = EventLog::read_all(log_path);

        ReplayResult result;
        for (const auto& cmd : commands) {
            if (cmd.type == CommandType::AddOrder) {
                auto fills = result.book.add(cmd.order);
                result.fills.insert(result.fills.end(),
                                    fills.begin(), fills.end());
            } else if (cmd.type == CommandType::CancelOrder) {
                result.book.cancel(cmd.cancel_id);
            }
        }
        return result;
    }

private:
    struct UserClientKey {
        UserId        user_id;
        ClientOrderId client_order_id;

        bool operator==(const UserClientKey& other) const = default;
    };

    struct UserClientKeyHash {
        std::size_t operator()(const UserClientKey& k) const noexcept {
            std::size_t h1 = std::hash<uint64_t>{}(k.user_id);
            std::size_t h2 = std::hash<uint64_t>{}(k.client_order_id);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
        }
    };

    void cleanup_client_mapping(OrderId id) {
        auto it = order_to_client_key_.find(id);
        if (it != order_to_client_key_.end()) {
            client_to_order_id_.erase(it->second);
            order_to_client_key_.erase(it);
        }
    }

    EventLog  log_;
    OrderBook book_;
    OrderId   next_order_id_{1};

    std::unordered_map<UserClientKey, OrderId, UserClientKeyHash> client_to_order_id_;
    std::unordered_map<OrderId, UserClientKey> order_to_client_key_;
};

} // namespace orderbook