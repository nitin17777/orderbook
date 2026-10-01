# Order Book - Specification

## Overview
A price-time priority matching engine supporting limit orders, market orders,
and cancellations. The engine is single-threaded and deterministic: given the
same sequence of commands, it always produces the same sequence of fills.

---

## Order Types

### Limit Order
- Has a price and a quantity.
- Rests in the book if it cannot be immediately filled.
- Matches against the opposite side at its limit price or better.

### Market Order
- Has no price, it takes the best available price on the opposite side.
- Never rests in the book.
- If liquidity is insufficient, fills what it can and the remainder is cancelled.

### Cancel
- Removes a resting limit order by its OrderId.
- Cancelling a non-existent or already-filled order is silently ignored.

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

## What is Out of Scope (for now)
- IOC / FOK order types
- Order modification (cancel-replace)
- Networking / wire protocol
- Persistence (added in a later milestone)
- Multi-threaded access