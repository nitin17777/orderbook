#pragma once

#include "orderbook/book.hpp"
#include "orderbook/event_log.hpp"
#include "orderbook/events.hpp"
#include "orderbook/types.hpp"

#include <chrono>
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

    // ── add (with event sink) ────────────────────────────────────────────────
    //
    // Submit a new order.  Validates input before any book mutation or logging.
    // Emits events into `sink` in canonical order:
    //   OrderRejected            — on any validation failure (book unchanged)
    //   OrderAccepted [Fill…]    — on success (one Fill per matched maker)
    //
    // Sequence numbers across all engine operations are gapless and monotonic.
    template<EventSink Sink>
    OrderResult add(Order order, Sink& sink) {
        const Timestamp now = engine_clock();

        // 1. Validation phase (zero book mutation / zero log writes on rejection)
        auto reject = [&](RejectReason reason) -> OrderResult {
            Event ev{};
            ev.rejected.header = make_header(EventTag::OrderRejected, now);
            ev.rejected.order_id = order.id;
            ev.rejected.reason   = reason;
            sink(ev);
            return OrderResult{false, reason, {}};
        };

        if (order.quantity == 0)
            return reject(RejectReason::InvalidQuantity);

        if (order.type == OrderType::Limit && order.price <= 0)
            return reject(RejectReason::InvalidPrice);

        if (order.id != INVALID_ORDER_ID) {
            if (book_.find(order.id) != nullptr)
                return reject(RejectReason::DuplicateOrderId);
        }

        if (order.user_id != INVALID_USER_ID && order.client_order_id != INVALID_CLIENT_ORDER_ID) {
            UserClientKey key{order.user_id, order.client_order_id};
            if (client_to_order_id_.find(key) != client_to_order_id_.end())
                return reject(RejectReason::DuplicateClientOrderId);
        }

        // 2. Order ID assignment
        if (order.id == INVALID_ORDER_ID) {
            order.id = next_order_id_++;
        } else {
            next_order_id_ = std::max(next_order_id_, order.id + 1);
        }

        // 3. Emit OrderAccepted before mutating state
        {
            Event ev{};
            ev.accepted.header     = make_header(EventTag::OrderAccepted, now);
            ev.accepted.order_id   = order.id;
            ev.accepted.side       = order.side;
            ev.accepted.order_type = order.type;
            sink(ev);
        }

        // 4. State transition: log first (source of truth), then match
        log_.append(Command::add(order));
        auto fills = book_.add(order);

        // 5. Emit one Fill event per matched maker
        for (const auto& f : fills) {
            Event ev{};
            ev.fill.header   = make_header(EventTag::Fill, now);
            ev.fill.taker_id = f.taker_id;
            ev.fill.maker_id = f.maker_id;
            ev.fill.price    = f.price;
            ev.fill.quantity = f.quantity;
            sink(ev);

            // Clean up fully-filled makers from client mapping
            if (book_.find(f.maker_id) == nullptr)
                cleanup_client_mapping(f.maker_id);
        }

        // 6. Index the resting order's client ID mapping
        if (book_.find(order.id) != nullptr) {
            if (order.user_id != INVALID_USER_ID && order.client_order_id != INVALID_CLIENT_ORDER_ID) {
                UserClientKey key{order.user_id, order.client_order_id};
                client_to_order_id_[key] = order.id;
                order_to_client_key_[order.id] = key;
            }
        }

        return OrderResult{true, RejectReason::None, std::move(fills)};
    }

    // Convenience overload — no event stream needed (e.g. existing call-sites).
    OrderResult add(Order order) {
        NullSink s;
        return add(std::move(order), s);
    }

    // ── cancel (with event sink) ─────────────────────────────────────────────
    //
    // Cancel a resting order by internal OrderId.  Validates existence and
    // ownership, then emits either CancelRejected or OrderCancelled.
    template<EventSink Sink>
    CancelResult cancel(OrderId id, UserId user_id, Sink& sink) {
        const Timestamp now = engine_clock();
        const Order* existing = book_.find(id);

        auto reject = [&](RejectReason reason) -> CancelResult {
            Event ev{};
            ev.cancel_rejected.header   = make_header(EventTag::CancelRejected, now);
            ev.cancel_rejected.order_id = id;
            ev.cancel_rejected.reason   = reason;
            sink(ev);
            return CancelResult{false, reason};
        };

        if (existing == nullptr)
            return reject(RejectReason::UnknownOrder);

        if (user_id != INVALID_USER_ID && existing->user_id != INVALID_USER_ID && existing->user_id != user_id)
            return reject(RejectReason::Unauthorized);

        const Quantity remaining = existing->open_quantity();

        // Log before mutating book (event-sourcing invariant)
        log_.append(Command::cancel(id));
        book_.cancel(id);
        cleanup_client_mapping(id);

        Event ev{};
        ev.cancelled.header        = make_header(EventTag::OrderCancelled, now);
        ev.cancelled.order_id      = id;
        ev.cancelled.remaining_qty = remaining;
        sink(ev);

        return CancelResult{true, RejectReason::None};
    }

    // Convenience overload — original signature, no event stream.
    CancelResult cancel(OrderId id, UserId user_id = INVALID_USER_ID) {
        NullSink s;
        return cancel(id, user_id, s);
    }

    // Cancel an order by user_id and client_order_id (with optional sink)
    template<EventSink Sink>
    CancelResult cancel_by_client_id(UserId user_id, ClientOrderId client_order_id, Sink& sink) {
        if (user_id == INVALID_USER_ID || client_order_id == INVALID_CLIENT_ORDER_ID) {
            // Emit CancelRejected even for the client-id path
            const Timestamp now = engine_clock();
            Event ev{};
            ev.cancel_rejected.header   = make_header(EventTag::CancelRejected, now);
            ev.cancel_rejected.order_id = INVALID_ORDER_ID;
            ev.cancel_rejected.reason   = RejectReason::UnknownOrder;
            sink(ev);
            return CancelResult{false, RejectReason::UnknownOrder};
        }

        UserClientKey key{user_id, client_order_id};
        auto it = client_to_order_id_.find(key);
        if (it == client_to_order_id_.end()) {
            const Timestamp now = engine_clock();
            Event ev{};
            ev.cancel_rejected.header   = make_header(EventTag::CancelRejected, now);
            ev.cancel_rejected.order_id = INVALID_ORDER_ID;
            ev.cancel_rejected.reason   = RejectReason::UnknownOrder;
            sink(ev);
            return CancelResult{false, RejectReason::UnknownOrder};
        }

        return cancel(it->second, user_id, sink);
    }

    CancelResult cancel_by_client_id(UserId user_id, ClientOrderId client_order_id) {
        NullSink s;
        return cancel_by_client_id(user_id, client_order_id, s);
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

    // ── Private helpers ──────────────────────────────────────────────────────

    // Logical clock: nanoseconds since engine construction (monotonically
    // increasing). Uses std::chrono::steady_clock — not wall time.
    Timestamp engine_clock() const {
        using namespace std::chrono;
        return static_cast<Timestamp>(
            duration_cast<nanoseconds>(steady_clock::now() - epoch_).count());
    }

    // Build an EventHeader, advancing the gapless sequence number.
    EventHeader make_header(EventTag tag, Timestamp ts) {
        EventHeader h{};
        h.seq         = next_seq_++;
        h.engine_time = ts;
        h.tag         = tag;
        return h;
    }

    EventLog  log_;
    OrderBook book_;
    OrderId   next_order_id_{1};
    uint64_t  next_seq_{1};  // gapless event sequence number; first event = 1

    // Epoch for the logical clock — set at engine construction
    std::chrono::steady_clock::time_point epoch_{std::chrono::steady_clock::now()};

    std::unordered_map<UserClientKey, OrderId, UserClientKeyHash> client_to_order_id_;
    std::unordered_map<OrderId, UserClientKey> order_to_client_key_;
};

} // namespace orderbook