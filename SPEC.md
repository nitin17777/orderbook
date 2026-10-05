# Order Book - Specification

## Overview
A price-time priority matching engine supporting limit orders, market orders,
and cancellations. The engine is single-threaded and deterministic: given the
same sequence of commands, it always produces the same sequence of fills.

---

## Order Types & Time-in-Force (TIF)

### Time-in-Force Semantics
- **GTC (Good 'Til Cancelled)**: Default execution policy. Fills available matching quantity immediately and rests any unfilled remainder in the order book.
- **IOC (Immediate Or Cancel)**: Fills whatever quantity is immediately available at or within the limit price; any unfilled tail is cancelled immediately without resting in the book (emits an `OrderCancelled` event for the remainder).
- **FOK (Fill Or Kill)**: Either executes the entire order quantity across available price levels immediately, or executes nothing (0 fills) and cancels the entire order immediately without mutating the order book state.
- **Market Orders**: Market orders execute against best available opposite liquidity up to their requested quantity; unfilled remainders are cancelled immediately without resting (behaving consistently with IOC semantics).

### Limit Order
- Has a price, quantity, and Time-in-Force (`GTC`, `IOC`, or `FOK`).
- Matches against the opposite side at its limit price or better.
- Rests in the book only if `TIF == GTC` and has an unfilled open quantity.

### Market Order
- Has no price, takes the best available price on the opposite side.
- Never rests in the book (IOC semantics). If liquidity is insufficient, fills what it can and cancels the remainder. If `FOK` is specified, fills the entire quantity or kills the order.

### Cancel
- Removes a resting limit order by its OrderId.
- Cancelling a non-existent or already-filled order is rejected cleanly.

---

## Matching Rule: Price-Time Priority
1. Best price first - bids match highest price first, asks match lowest price first.
2. Among orders at the same price - earliest arrival (FIFO) matches first.

---

## Core Types

| Type          | Underlying type     | Notes                              |
|---------------|---------------------|------------------------------------|
| OrderId       | uint64_t            | Monotonically increasing, unique   |
| UserId        | uint64_t            | Participant / account identifier   |
| ClientOrderId | uint64_t            | Client-assigned per-user ID        |
| Price         | int64_t             | In ticks, NOT floating point       |
| Quantity      | uint64_t            | Always positive                    |
| Side          | enum (Buy / Sell)   |                                    |
| Timestamp     | uint64_t            | Nanoseconds since epoch (logical)  |

**Why integer price?** Floating point arithmetic is non-deterministic across
platforms and has rounding error. Real exchanges use integer ticks with a
known tick size. We do the same.

---

## Memory Layout & Cache Alignment

The `Order` struct is intentionally engineered to fit inside **exactly 64 bytes** (one CPU cache line on x86-64 and ARM64):
- 7 x 8-byte scalar fields (`id`, `user_id`, `client_order_id`, `price`, `quantity`, `filled`, `timestamp`) = 56 bytes
- 3 x 1-byte enums (`side`, `type`, `status`) = 3 bytes
- 5-byte explicit padding (`reserved[5]`) = 5 bytes
- **Total: 64 bytes**.

Fitting an `Order` within a single cache line guarantees that order traversals and pool updates never suffer from split-cache-line memory access penalties. `Order` is also guaranteed to be standard-layout and trivially copyable for raw binary event logging.

---

## Order Identity and ID Assignment

1. **Internal `OrderId`**:
   - Engine-assigned, monotonically increasing 64-bit integer (`1, 2, 3, ...`).
   - If a caller supplies `INVALID_ORDER_ID` (`0`), the engine assigns `next_order_id_++`.
   - If a caller supplies an explicit `OrderId != 0`, the engine checks for uniqueness before accepting.
2. **Client Identity (`UserId` + `ClientOrderId`)**:
   - Clients supply their `UserId` and their own `ClientOrderId`.
   - The engine enforces that `(UserId, ClientOrderId)` is strictly unique across all active resting orders in that user's namespace.
   - Distinct users can safely use the same `ClientOrderId` without collision.
   - The engine maintains bidirectional index mapping for O(1) cancel lookup and cleans up mappings when orders become terminal (fully filled or cancelled).

---

## Input Validation & Error Handling (Zero-Exception)

Validation occurs exclusively in the `Engine` layer **before** any book mutation or event log append:
- **Price**: Limit orders require `price > 0`. Non-positive limit prices are rejected with `RejectReason::InvalidPrice`. Market orders ignore price.
- **Quantity**: Orders require `quantity > 0`. Zero quantities are rejected with `RejectReason::InvalidQuantity`.
- **Duplicates**: Duplicate active `OrderId` (`RejectReason::DuplicateOrderId`) and duplicate `(UserId, ClientOrderId)` (`RejectReason::DuplicateClientOrderId`) are rejected.
- **Cancellations**: Unknown orders are rejected with `RejectReason::UnknownOrder`. Cancellations where caller `UserId` does not match the order owner are rejected with `RejectReason::Unauthorized`.

**Zero-Exception Hot Path**: Rejections return an `OrderResult` or `CancelResult` with an error code enum (`RejectReason`). On rejection, the engine's internal state, order book, and event log remain 100% untouched and uncorrupted.

---

## Order Lifecycle
- An order that is never matched and is later cancelled goes: Accepted → Cancelled.
- An order partially filled and then cancelled goes: Accepted → PartiallyFilled → Cancelled.
- A fully filled order never appears in the book.

---

## Fill Event
Every time two orders match, a Fill is emitted:

| Field        | Type      |
|--------------|-----------|
| taker_id     | OrderId   |
| maker_id     | OrderId   |
| price        | Price     |
| quantity     | Quantity  |

- **Maker** = the resting order that was already in the book.
- **Taker** = the incoming order that triggered the match.
- Fill price is always the **maker's price** (standard exchange convention).

---

## Edge Cases (must be tested)

1. Market order against an empty book → fills nothing, order cancelled.
2. Limit order that crosses the spread → treated as aggressive, matches immediately.
3. Partial fill → remainder rests in book (limit) or is cancelled (market).
4. Cancel of unknown OrderId → returns `UnknownOrder` rejection in Engine.
5. Cancel of already fully filled order → returns `UnknownOrder` rejection in Engine.
6. Cancel by unauthorized user → returns `Unauthorized` rejection in Engine.
7. Zero quantity or non-positive limit price → returns rejection and leaves state unchanged.
8. Multiple fills from one large incoming order → emits one Fill per matched order.
9. Crossed book (ask < bid already in book) → should never happen if engine is correct; treat as an assertion.

---

## Event Model (Task 2.1)

### Commands vs Events

| Layer | File | Purpose |
|---|---|---|
| Command | `command.hpp` | *Intent* — raw input (Add, Cancel) stored in the binary log |
| Event | `events.hpp` | *Fact* — what the engine actually did, with sequence numbers |

Downstream consumers (market-data feed, gateway, ledger, audit) subscribe to
the **event stream** and never need to touch matching logic.

### Event Types

| Tag | Struct | When emitted |
|---|---|---|
| `OrderAccepted` | `EventOrderAccepted` | Order passes validation; precedes any Fill events |
| `OrderRejected` | `EventOrderRejected` | Validation fails; engine state unchanged |
| `Fill` | `EventFill` | One per matched maker; taker price = maker price |
| `OrderCancelled` | `EventOrderCancelled` | Resting order successfully removed |
| `CancelRejected` | `EventCancelRejected` | Cancel failed (UnknownOrder / Unauthorized) |
| `OrderModified` | `EventOrderModified` | Order price/quantity modified |

### Ordering Guarantees

1. Events for a single command are **contiguous** — no interleaving between commands.
2. Sequence numbers are **monotonically increasing with no gaps** (1, 2, 3, …).
3. Within one command, sub-events appear in a fixed canonical order:
   - `add` accepted → `OrderAccepted` → `[Fill…]`
   - `add` rejected → `OrderRejected`
   - `cancel` success → `OrderCancelled`
   - `cancel` failed  → `CancelRejected`
   - `modify` success → `OrderModified` → `[Fill…]`
   - `modify` failed  → `OrderRejected`
4. Replaying the same command sequence always produces **byte-identical events**.

### Sink Contract (allocation-free)

```cpp
// Any callable matching this concept is a valid sink:
template<typename S>
concept EventSink = requires(S& s, const Event& e) { { s(e) }; };

// Built-in sinks:
NullSink{}      // discards all events — zero cost; use in benchmarks
VectorSink{}    // collects into std::vector<Event> — use in tests / replay
```

The engine calls the sink **synchronously** in sequence-number order.
No heap allocation is performed per `add()` / `cancel()` / `modify()` call.

### Binary Stability

Every event struct is **fixed-size** and **trivially copyable** (enforced with
`static_assert`). The `EventHeader` is a common 24-byte prefix on every event.
The discriminator tag occupies a full `uint8_t`; values ≥ 6 are reserved.

### Event Sizes

| Struct | Size |
|---|---|
| `EventHeader` | 24 bytes |
| `EventOrderAccepted` | 40 bytes |
| `EventOrderRejected` | 40 bytes |
| `EventFill` | 56 bytes |
| `EventOrderCancelled` | 40 bytes |
| `EventCancelRejected` | 40 bytes |
| `EventOrderModified` | 56 bytes |
| `Event` (union) | 56 bytes |

---

## What is Out of Scope (for now)
- Persistence (added in a later milestone)
- Multi-threaded access