# Task 4.4: Reference Client & End-to-End Demonstration Walkthrough

This document provides a complete guide and walkthrough of the OrderBook reference client (`client/client.py`) and the scripted end-to-end demonstration scenario (`client/demo_scenario.py`).

---

## 1. Reference Client Overview

The reference client is an asynchronous Python client built on top of `websockets` that interfaces with the C++ `orderbook_gateway` binary over `ws://<host>:<port>`.

### Features
- **Interactive Shell**: Place limit/market orders, cancel orders by ID, request snapshots, and inspect local book state in real time.
- **Colorized Live Ticker**: Pretty-prints private execution reports (`order_accepted`, `execution`, `order_cancelled`, `order_rejected`) and public streams (`public.bbo`, `public.trades`, `public.depth_l2`).
- **Real-Time L2 Ladder**: Maintains a client-side aggregated depth book from initial snapshot + incremental deltas (`UPD` / `DEL`).

### Quick Start
```powershell
# Start the Gateway Server:
.\build\orderbook_gateway.exe 9001 gateway.log 127.0.0.1

# Launch the interactive client as User 1 with auto-subscriptions:
python client/client.py --user 1 --sub
```

---

## 2. End-to-End Walkthrough Scenario

The scripted demonstration (`client/demo_scenario.py`) connects three independent participants to the gateway and performs a 6-phase market simulation.

### Running the Demo Script:
```powershell
python client/demo_scenario.py 127.0.0.1 9001
```

---

## 3. Walkthrough Phases & State Transitions

### Phase 1: Multi-Participant Connection & Subscriptions
- **Participants**:
  - `Maker (User #1)`: Seeds liquidity across price levels.
  - `Taker (User #2)`: Executes crossing orders and market sweeps.
  - `Observer (User #99)`: Subscribes to `public.bbo`, `public.trades`, and `public.depth_l2`.
- **Protocol Exchange**:
  ```json
  --> {"req_id": "sub-all", "type": "subscribe", "payload": {"channels": ["public.bbo", "public.trades", "public.depth_l2"]}}
  <-- {"type": "subscribed", "status": "ok", "payload": {"channels": [...]}}
  <-- {"type": "depth_snapshot", "channel": "public.depth_l2", "payload": {"bids": [], "asks": []}}
  ```

---

### Phase 2: Seeding Depth Ladder
- **Maker Actions**:
  - Submits 5 Buy orders: 10 @ 95, 10 @ 96, 10 @ 97, 10 @ 98, 10 @ 99.
  - Submits 5 Sell orders: 10 @ 101, 10 @ 102, 10 @ 103, 10 @ 104, 10 @ 105.
- **Market Data Feed**:
  - Emits 10 incremental `depth_update` messages (one per price level).
  - Emits BBO updates converging to: `Best Bid: 99 (10) <-> Best Ask: 101 (10) | Spread: 2`.

```text
  Current Aggregated L2 Depth Ladder:
     Qty (Bid) |  Price   | Qty (Ask)   
  ----------------------------------
               |   105    | 10          
               |   104    | 10          
               |   103    | 10          
               |   102    | 10          
               |   101    | 10          
               |  ======  |             
            10 |    99    |             
            10 |    98    |             
            10 |    97    |             
            10 |    96    |             
            10 |    95    |             
  ----------------------------------
```

---

### Phase 3: Partial Crossing Fill & Privacy Boundary
- **Taker Action**: Submits `Buy 4 @ 101`.
- **Matching & Execution**:
  - Taker order matches 4 lots against the resting maker ask at 101.
  - Remaining ask depth at 101 becomes `6`.
- **Private Stream (Taker Session)**:
  ```json
  <-- {"type": "order_accepted", "payload": {"order_id": 27, "side": "buy", "price": 101, "quantity": 4}}
  <-- {"type": "execution", "payload": {"role": "taker", "match_price": 101, "filled_qty": 4, "remaining_qty": 0, "order_status": "filled"}}
  ```
- **Public Stream (Observer Session)**:
  - `trade`: `{"price": 101, "quantity": 4, "aggressor_side": "buy"}` *(Notice: 0 private user/order IDs)*.
  - `depth_update`: `{"side": "sell", "price": 101, "new_qty": 6}`.
  - `bbo`: `{"bid_price": 99, "bid_qty": 10, "ask_price": 101, "ask_qty": 6}`.

---

### Phase 4: Multi-Level Market Sweep
- **Taker Action**: Submits aggressive `Buy 20 @ 103`.
- **Multi-Level Execution**:
  1. Sweeps remaining 6 lots @ 101 -> Level 101 completely exhausted (`new_qty: 0`, removed from book).
  2. Sweeps all 10 lots @ 102 -> Level 102 completely exhausted (`new_qty: 0`, removed from book).
  3. Sweeps 4 lots @ 103 -> Level 103 reduced from 10 to 6 lots (`new_qty: 6`).
- **Reports Received**:
  - Taker receives 3 distinct `execution` reports as each level is consumed.
  - Public trade stream receives 3 individual `trade` records.
  - BBO shifts instantly to: `Best Bid: 99 (10) <-> Best Ask: 103 (6) | Spread: 4`.

```text
  Current Aggregated L2 Depth Ladder:
     Qty (Bid) |  Price   | Qty (Ask)   
  ----------------------------------
               |   105    | 10          
               |   104    | 10          
               |   103    | 6           
               |  ======  |             
            10 |    99    |             
            10 |    98    |             
            10 |    97    |             
            10 |    96    |             
            10 |    95    |             
  ----------------------------------
```

---

### Phase 5: Order Cancellation & Book Update
- **Maker Action**: Cancels resting order.
- **Engine Processing**:
  - Book removes resting quantity incrementally.
  - Emits `order_cancelled` ack to maker.
  - Emits `depth_update` and updated `bbo` to public observers.

---

### Phase 6: Protocol Error Handling & Recovery
- **Zero/Negative Price**:
  - Request: `{"price": 0, "quantity": 10}`
  - Response: `{"type": "error", "error": {"code": "INVALID_PRICE", "message": "Limit price must be strictly positive"}}`
- **Duplicate Client Order ID**:
  - Request with already-used `client_order_id`.
  - Response: `{"type": "order_rejected", "error": {"code": "DUPLICATE_CLIENT_ORDER_ID"}}`
- **Malformed JSON**:
  - Payload: `{invalid json syntax`
  - Response: `{"type": "error", "error": {"code": "MALFORMED_JSON"}}`
  - **Result**: Server survives with zero memory corruption, crashes, or stalled event loop.
