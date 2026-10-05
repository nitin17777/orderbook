#pragma once

// ────────────────────────────────────────────────────────────────────────────
// events.hpp  —  Rich event model for the Order Book Engine
//
// DESIGN PHILOSOPHY
// -----------------
// Commands (command.hpp) describe *intent* — what the caller asked for.
// Events  (this file)   describe *fact*   — what the engine actually did.
//
// Every engine operation produces one or more events in a strictly ordered,
// gapless sequence.  Downstream consumers (market-data feed, audit log,
// ledger, gateway) subscribe to this stream and never need to touch matching
// logic.
//
// ORDERING GUARANTEES
// -------------------
//  1. Events for a single command are contiguous — no interleaving.
//  2. Sequence numbers are monotonically increasing with NO gaps (1, 2, 3, …).
//  3. Within one command the sub-events appear in a fixed canonical order:
//       add accepted  → [Fill…]   → (nothing more)
//       add rejected  → (nothing more)
//       cancel acked  → (nothing more)
//       cancel rejected → (nothing more)
//  4. Replaying the same command sequence always produces byte-identical events.
//
// ALLOCATION POLICY
// -----------------
// The engine writes events into a caller-supplied *sink* — any callable that
// accepts (const Event&).  No heap allocation is performed per-call; the sink
// decides whether to buffer, persist, or discard.
//
// BINARY STABILITY
// ----------------
// Every event type is fixed-size and trivially copyable so that it can be
// written directly to a binary log with a single write() call.  The
// EventTag discriminator occupies a full uint8_t field; values > 4 are
// reserved for future use (e.g. OrderModified).
// ────────────────────────────────────────────────────────────────────────────

#include "orderbook/types.hpp"

#include <cstdint>
#include <type_traits>
#include <vector>

namespace orderbook {

// ── Discriminator ────────────────────────────────────────────────────────────

enum class EventTag : uint8_t {
    OrderAccepted  = 0,  // limit or market order entered the engine
    OrderRejected  = 1,  // order failed validation — book untouched
    Fill           = 2,  // two orders matched; emitted once per resting maker
    OrderCancelled = 3,  // resting order removed from book
    CancelRejected = 4,  // cancel request failed (unknown / unauthorized)
    OrderModified  = 5,  // RESERVED — not yet implemented
};

// ── Common header present in every event ─────────────────────────────────────
//
// Every event begins with this 24-byte prefix so consumers can read the
// discriminator and sequence number before inspecting the full payload.
//
// Layout (24 bytes):
//   seq          8 bytes   — gapless sequence number; first event is 1
//   engine_time  8 bytes   — engine-assigned logical timestamp (nanoseconds)
//   tag          1 byte    — EventTag discriminator
//   _pad         7 bytes   — explicit padding; reserved, must be zero
struct EventHeader {
    uint64_t  seq;           // monotonically increasing, no gaps; first = 1
    Timestamp engine_time;   // nanosecond logical clock assigned by engine
    EventTag  tag;
    uint8_t   _pad[7]{};
};
static_assert(sizeof(EventHeader) == 24, "EventHeader must be 24 bytes");
static_assert(std::is_trivially_copyable_v<EventHeader>);

// ── Event types ───────────────────────────────────────────────────────────────

// Emitted when an order passes all validation and has been accepted into the
// engine.  Immediately precedes any Fill events for the same command.
//
// For a market order that is immediately fully filled, OrderAccepted is still
// emitted — it records that the order was *received* by the engine.
//
// Layout: 24 (header) + 8 + 1 + 1 + 6 = 40 bytes
struct EventOrderAccepted {
    EventHeader header;
    OrderId     order_id;       // engine-assigned id
    Side        side;
    OrderType   order_type;
    TimeInForce tif;
    uint8_t     _pad[5]{};
};
static_assert(sizeof(EventOrderAccepted) == 40);
static_assert(std::is_trivially_copyable_v<EventOrderAccepted>);

// Emitted when an order fails validation.  The engine's state (book + log)
// is guaranteed to be 100% unchanged when this event is emitted.
//
// Layout: 24 (header) + 8 + 1 + 7 = 40 bytes
struct EventOrderRejected {
    EventHeader  header;
    OrderId      order_id;   // caller-supplied id (may be INVALID_ORDER_ID)
    RejectReason reason;
    uint8_t      _pad[7]{};
};
static_assert(sizeof(EventOrderRejected) == 40);
static_assert(std::is_trivially_copyable_v<EventOrderRejected>);

// Emitted once per matched maker for every taker command that produces a trade.
// Multiple Fill events for a single taker are emitted in FIFO price-time order
// (the order in which the makers were matched).
//
// Fill price is always the maker's price — standard exchange convention.
//
// Layout: 24 (header) + 8 + 8 + 8 + 8 = 56 bytes
struct EventFill {
    EventHeader header;
    OrderId     taker_id;   // incoming order that triggered the match
    OrderId     maker_id;   // resting order that was already in the book
    Price       price;      // always the maker's price
    Quantity    quantity;   // matched quantity in this fill
};
static_assert(sizeof(EventFill) == 56);
static_assert(std::is_trivially_copyable_v<EventFill>);

// Emitted after a resting order is successfully removed from the book.
//
// Layout: 24 (header) + 8 + 8 = 40 bytes
struct EventOrderCancelled {
    EventHeader header;
    OrderId     order_id;
    Quantity    remaining_qty;  // quantity that was still open at cancel time
};
static_assert(sizeof(EventOrderCancelled) == 40);
static_assert(std::is_trivially_copyable_v<EventOrderCancelled>);

// Emitted when a cancel request is rejected (unknown order or not authorised).
//
// Layout: 24 (header) + 8 + 1 + 7 = 40 bytes
struct EventCancelRejected {
    EventHeader  header;
    OrderId      order_id;
    RejectReason reason;
    uint8_t      _pad[7]{};
};
static_assert(sizeof(EventCancelRejected) == 40);
static_assert(std::is_trivially_copyable_v<EventCancelRejected>);

// Emitted when a resting order is modified.
//
// Layout: 24 (header) + 8 + 8 + 8 + 1 + 7 = 56 bytes
struct EventOrderModified {
    EventHeader header;
    OrderId     order_id;
    Price       new_price;
    Quantity    new_quantity;
    Side        side;
    uint8_t     _pad[7]{};
};
static_assert(sizeof(EventOrderModified) == 56);
static_assert(std::is_trivially_copyable_v<EventOrderModified>);

// ── Tagged union wrapper ──────────────────────────────────────────────────────
//
// A single Event value that can hold any of the above types.
// Consumers branch on event.tag() (or event.header.tag) to downcast.
//
// The union is padded to the size of the largest member (EventFill = 56 bytes).
//
// Usage:
//   if (e.tag() == EventTag::Fill) {
//       const auto& fill = e.as<EventFill>();
//   }
union Event {
    EventHeader         header;          // always valid — read tag here first
    EventOrderAccepted  accepted;
    EventOrderRejected  rejected;
    EventFill           fill;
    EventOrderCancelled cancelled;
    EventCancelRejected cancel_rejected;
    EventOrderModified  modified;

    // Convenience typed accessor — no tag check, caller's responsibility.
    template<typename T>
    const T& as() const { return reinterpret_cast<const T&>(*this); }

    template<typename T>
    T& as() { return reinterpret_cast<T&>(*this); }

    EventTag tag() const { return header.tag; }
};
static_assert(sizeof(Event) == 56, "Event union must be 56 bytes (size of EventFill)");
static_assert(std::is_trivially_copyable_v<Event>);

// ── Sink concept ─────────────────────────────────────────────────────────────
//
// An EventSink is any callable that satisfies:
//   void sink(const Event& e);
//
// Examples:
//   - A lambda that writes e to a binary file
//   - A std::vector<Event> push_back wrapper (see VectorSink below)
//   - A no-op sink for benchmarks (see NullSink below)
//
// The engine calls the sink synchronously and in sequence-number order.
// The sink must not throw — use noexcept wrappers around fallible I/O.
template<typename S>
concept EventSink = requires(S& s, const Event& e) {
    { s(e) };
};

// ── NullSink ─────────────────────────────────────────────────────────────────
// Discards all events.  Zero cost — used in benchmarks and call-sites that do
// not need the event stream.
struct NullSink {
    void operator()(const Event&) const noexcept {}
};
static_assert(EventSink<NullSink>);

// ── VectorSink ───────────────────────────────────────────────────────────────
// Collects events into a std::vector<Event>.  Convenient for tests and for
// in-process replay verification.
struct VectorSink {
    std::vector<Event> events;
    void operator()(const Event& e) { events.push_back(e); }
};
static_assert(EventSink<VectorSink>);

} // namespace orderbook
