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

| Type       | Underlying type     | Notes                              |
|------------|---------------------|------------------------------------|
| OrderId    | uint64_t            | Monotonically increasing, unique   |
| Price      | int64_t             | In ticks, NOT floating point       |
| Quantity   | uint64_t            | Always positive                    |
| Side       | enum (Buy / Sell)   |                                    |
| Timestamp  | uint64_t            | Nanoseconds since epoch (logical)  |

**Why integer price?** Floating point arithmetic is non-deterministic across
platforms and has rounding error. Real exchanges use integer ticks with a
known tick size. We do the same.

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
4. Cancel of unknown OrderId → no-op, no error.
5. Cancel of already fully filled order → no-op, no error.
6. Multiple fills from one large incoming order → emits one Fill per matched order.
7. Crossed book (ask < bid already in book) → should never happen if engine is correct; treat as an assertion.

---

## What is Out of Scope (for now)
- IOC / FOK order types
- Order modification (cancel-replace)
- Networking / wire protocol
- Persistence (added in a later milestone)
- Multi-threaded access