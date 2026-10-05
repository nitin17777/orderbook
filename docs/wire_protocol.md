# Gateway Wire Protocol Specification (v1.0)

This document specifies the communication contract between clients (traders, bots, market makers, market data consumers) and the Order Book Gateway.

---

## 1. Overview & Architectural Principles

### 1.1 Transport & Connection Model
- **Transport**: WebSockets (RFC 6455) or Length-Prefixed / Newline-Delimited TCP.
- **Message Framing**: Each frame contains exactly one JSON message payload (UTF-8 encoded).
- **Session Types**:
  - **Public Market Data Session**: Anonymous, read-only. Subscribes to public market data channels.
  - **Trading / Order Session**: Authenticated with a `user_id` / API token. Can submit, modify, and cancel orders and receives private execution reports.

### 1.2 Encoding Independence
While this specification standardizes **JSON** as the baseline human-readable wire format for debuggability, every message schema is strictly mapped to fixed-layout structs so a binary encoding (e.g., SBE, FlatBuffers, or binary framing) can map 1-to-1 to identical fields without architectural redesign.

### 1.3 Channel Taxonomy
Messages and events are segmented into distinct logical channels:

| Channel Name | Visibility | Description |
| :--- | :--- | :--- |
| `public.bbo` | **Public** | Top-of-book quotes (Best Bid / Best Offer, spread, mid-price). |
| `public.trades` | **Public** | Anonymized public trade feed (execution price, qty, aggressor side). Zero participant IDs. |
| `public.depth_l2` | **Public** | Aggregated price-level depth (snapshots and incremental deltas). |
| `private.orders` | **Private** | Order lifecycle acknowledgements, rejects, and cancellations for the authenticated user. |
| `private.fills` | **Private** | Private execution reports and trade fills for the authenticated user. |

---

## 2. Message Envelope & Wire Format

All client-to-server requests and server-to-client responses share a standardized top-level envelope.

### 2.1 Client Request Envelope
```json
{
  "req_id": "req-1001",
  "type": "place_order",
  "channel": "trading",
  "timestamp": 1712234000000000,
  "payload": {}
}
```

#### Field Definitions:
- `req_id` (`string`, required): Client-generated correlation identifier. Mirrored in the server's immediate response.
- `type` (`string`, required): Message verb (e.g., `place_order`, `cancel_order`, `subscribe`, `unsubscribe`, `get_snapshot`).
- `channel` (`string`, optional): Target subsystem (`trading`, `market_data`, `system`).
- `timestamp` (`uint64`, optional): Client epoch timestamp in nanoseconds or microseconds.
- `payload` (`object`, required): Command-specific payload.

---

### 2.2 Server Response Envelope
```json
{
  "req_id": "req-1001",
  "type": "ack",
  "channel": "private.orders",
  "seq": 10452,
  "timestamp": 1712234000005120,
  "status": "ok",
  "payload": {}
}
```

#### Field Definitions:
- `req_id` (`string`, optional): Matches client `req_id` if emitted in direct response to a request; `null` for unsolicited push streams.
- `type` (`string`, required): Server event type (e.g., `ack`, `reject`, `execution`, `bbo`, `trade`, `depth_update`, `depth_snapshot`).
- `channel` (`string`, required): Source channel (`public.bbo`, `public.trades`, `public.depth_l2`, `private.orders`, `private.fills`).
- `seq` (`uint64`, required): Monotonically increasing, gapless sequence number for the stream.
- `timestamp` (`uint64`, required): Engine logical nanosecond timestamp when the event occurred.
- `status` (`string`, required): `"ok"` or `"error"`.
- `payload` (`object`, optional): Event payload data on success.
- `error` (`object`, optional): Error details on failure (`code`, `message`).

---

## 3. Client Requests (Trading & Subscription)

### 3.1 Place Order (`place_order`)
Submits a new Limit or Market order.

#### Request Schema:
```json
{
  "req_id": "cl-ord-001",
  "type": "place_order",
  "payload": {
    "user_id": 101,
    "client_order_id": 5001,
    "side": "buy",
    "order_type": "limit",
    "time_in_force": "gtc",
    "price": 10050,
    "quantity": 10
  }
}
```

#### Parameters:
- `user_id` (`uint64`, required): Participant identifier.
- `client_order_id` (`uint64`, required): Per-user unique client order ID.
- `side` (`string`, required): `"buy"` or `"sell"`.
- `order_type` (`string`, required): `"limit"` or `"market"`.
- `time_in_force` (`string`, optional, default `"gtc"`): `"gtc"`, `"ioc"`, or `"fok"`.
- `price` (`int64`, required for limit): Price in integer ticks (must be `> 0` for limit orders; ignored for market orders).
- `quantity` (`uint64`, required): Order size in integer lots (must be `> 0`).

---

### 3.2 Cancel Order (`cancel_order`)
Cancels an active resting order by internal `order_id` or by `(user_id, client_order_id)`.

#### Option A: Cancel by `order_id`
```json
{
  "req_id": "cl-cancel-001",
  "type": "cancel_order",
  "payload": {
    "user_id": 101,
    "order_id": 2049
  }
}
```

#### Option B: Cancel by `client_order_id`
```json
{
  "req_id": "cl-cancel-002",
  "type": "cancel_order",
  "payload": {
    "user_id": 101,
    "client_order_id": 5001
  }
}
```

---

### 3.3 Modify Order (`modify_order`)
Modifies the price and/or quantity of an active resting order by `order_id` or `(user_id, client_order_id)`.

#### Priority Rules:
- **Reducing quantity at unchanged price**: Retains queue priority (FIFO position preserved).
- **Increasing quantity or changing price**: Loses queue priority (moved to back of queue or aggressive cross). If new price crosses the spread, aggressive matching occurs immediately.

#### Option A: Modify by `order_id`
```json
{
  "req_id": "cl-mod-001",
  "type": "modify_order",
  "payload": {
    "user_id": 101,
    "order_id": 2049,
    "new_price": 10050,
    "new_quantity": 5
  }
}
```

#### Option B: Modify by `client_order_id`
```json
{
  "req_id": "cl-mod-002",
  "type": "modify_order",
  "payload": {
    "user_id": 101,
    "client_order_id": 5001,
    "new_price": 10050,
    "new_quantity": 15
  }
}
```

---

### 3.4 Channel Subscription (`subscribe`)
Subscribes the connection to one or more public or private streams.

```json
{
  "req_id": "sub-01",
  "type": "subscribe",
  "payload": {
    "channels": ["public.bbo", "public.trades", "public.depth_l2"]
  }
}
```

---

### 3.4 Request Depth Snapshot (`get_snapshot`)
Requests an immediate full L2 depth snapshot (used on connection startup or gap recovery).

```json
{
  "req_id": "snap-01",
  "type": "get_snapshot",
  "payload": {
    "channel": "public.depth_l2"
  }
}
```

---

## 4. Server Messages (Private Execution Reports)

Private execution reports are sent exclusively to the authenticated user owning the order.

### 4.1 Order Accepted (`order_accepted`)
Emitted immediately when an order passes input validation and is accepted into the engine.

```json
{
  "req_id": "cl-ord-001",
  "type": "order_accepted",
  "channel": "private.orders",
  "seq": 101,
  "timestamp": 1712234000100000,
  "status": "ok",
  "payload": {
    "order_id": 2049,
    "client_order_id": 5001,
    "user_id": 101,
    "side": "buy",
    "order_type": "limit",
    "price": 10050,
    "quantity": 10,
    "status": "accepted"
  }
}
```

---

### 4.2 Order Rejected (`order_rejected`)
Emitted when an order fails input validation. State is 100% untouched.

```json
{
  "req_id": "cl-ord-002",
  "type": "order_rejected",
  "channel": "private.orders",
  "seq": 102,
  "timestamp": 1712234000150000,
  "status": "error",
  "error": {
    "code": "INVALID_PRICE",
    "message": "Limit price must be strictly positive"
  },
  "payload": {
    "client_order_id": 5002,
    "user_id": 101
  }
}
```

---

### 4.3 Private Trade Fill (`execution`)
Emitted once per matched counterparty fill to the respective participants.

```json
{
  "req_id": "cl-ord-001",
  "type": "execution",
  "channel": "private.fills",
  "seq": 103,
  "timestamp": 1712234000200000,
  "status": "ok",
  "payload": {
    "order_id": 2049,
    "client_order_id": 5001,
    "user_id": 101,
    "side": "buy",
    "role": "taker",
    "match_price": 10050,
    "filled_qty": 6,
    "remaining_qty": 4,
    "order_status": "partially_filled"
  }
}
```

---

### 4.4 Order Cancelled (`order_cancelled`)
Emitted when an active resting order is successfully removed from the book.

```json
{
  "req_id": "cl-cancel-001",
  "type": "order_cancelled",
  "channel": "private.orders",
  "seq": 104,
  "timestamp": 1712234000300000,
  "status": "ok",
  "payload": {
    "order_id": 2049,
    "client_order_id": 5001,
    "user_id": 101,
    "remaining_qty": 4,
    "status": "cancelled"
  }
}
```

---

### 4.5 Order Modified (`order_modified`)
Emitted when an active resting order has been successfully modified in price and/or quantity.

```json
{
  "req_id": "cl-mod-001",
  "type": "order_modified",
  "channel": "private.orders",
  "seq": 105,
  "timestamp": 1712234000320000,
  "status": "ok",
  "payload": {
    "order_id": 2049,
    "client_order_id": 5001,
    "user_id": 101,
    "side": "buy",
    "new_price": 10050,
    "new_quantity": 5,
    "status": "modified"
  }
}
```

---

### 4.6 Cancel Rejected (`cancel_rejected`)
Emitted when a cancel request fails (e.g., unknown order ID or unauthorized caller).

```json
{
  "req_id": "cl-cancel-003",
  "type": "cancel_rejected",
  "channel": "private.orders",
  "seq": 106,
  "timestamp": 1712234000350000,
  "status": "error",
  "error": {
    "code": "UNKNOWN_ORDER",
    "message": "Order ID not found or already terminal"
  }
}
```

---

## 5. Server Messages (Public Market Data Streams)

> [!IMPORTANT]
> **Privacy Boundary**: Public messages are strictly sanitized. They NEVER contain `user_id`, `client_order_id`, or `order_id`.

### 5.1 Public Trade Stream (`trade`)
Emitted on channel `public.trades` whenever any trade execution occurs.

```json
{
  "type": "trade",
  "channel": "public.trades",
  "seq": 401,
  "timestamp": 1712234000200000,
  "status": "ok",
  "payload": {
    "price": 10050,
    "quantity": 6,
    "aggressor_side": "buy"
  }
}
```

#### Field Definitions:
- `price` (`int64`): Execution price.
- `quantity` (`uint64`): Match quantity.
- `aggressor_side` (`string`): `"buy"` if incoming taker was a buyer crossing the spread; `"sell"` if taker was a seller.

---

### 5.2 Best Bid / Offer (`bbo`)
Emitted on channel `public.bbo` whenever the top of book (best bid/ask price or total quantity) changes.

```json
{
  "type": "bbo",
  "channel": "public.bbo",
  "seq": 402,
  "timestamp": 1712234000205000,
  "status": "ok",
  "payload": {
    "bid_price": 10000,
    "bid_qty": 15,
    "ask_price": 10050,
    "ask_qty": 20,
    "spread": 50,
    "mid_price": 10025.0
  }
}
```
*Note: If a side is empty, `bid_price` / `ask_price` is `null` and `spread` / `mid_price` is `null`.*

---

### 5.3 Full Depth Snapshot (`depth_snapshot`)
Full L2 aggregated price level snapshot. Bids are sorted descending by price; asks ascending.

```json
{
  "req_id": "snap-01",
  "type": "depth_snapshot",
  "channel": "public.depth_l2",
  "seq": 850,
  "timestamp": 1712234000500000,
  "status": "ok",
  "payload": {
    "bids": [
      {"price": 10000, "qty": 15},
      {"price": 9950,  "qty": 40}
    ],
    "asks": [
      {"price": 10050, "qty": 20},
      {"price": 10100, "qty": 35}
    ]
  }
}
```

---

### 5.4 Incremental Depth Update (`depth_update`)
Emitted on channel `public.depth_l2` after every depth-altering book mutation.

```json
{
  "type": "depth_update",
  "channel": "public.depth_l2",
  "seq": 851,
  "prev_seq": 850,
  "timestamp": 1712234000520000,
  "status": "ok",
  "payload": {
    "side": "buy",
    "price": 10000,
    "new_qty": 25
  }
}
```

#### Field Definitions:
- `seq` (`uint64`): Unique sequence number for this delta.
- `prev_seq` (`uint64`): The sequence number of the preceding update the client must have applied.
- `side` (`string`): `"buy"` or `"sell"`.
- `price` (`int64`): The price level affected.
- `new_qty` (`uint64`): The new aggregate resting quantity at that level. **`new_qty == 0` means the level is deleted**.

---

## 6. Sequence Numbers, Resync & Gap Recovery

### 6.1 The Gapless Sequencing Invariant
Every stream (public or private) maintains monotonically increasing, gapless sequence numbers:
$$seq_n = seq_{n-1} + 1$$

### 6.2 Client Gap Detection Rule
When a client receives an incremental update (`depth_update`):
```text
if (delta.prev_seq == client.last_seq) {
    apply_delta(delta);
    client.last_seq = delta.seq;
} else {
    // GAP DETECTED: client missed (delta.prev_seq - client.last_seq) messages
    trigger_resync();
}
```

### 6.3 Resync State Machine Workflow
```
   [ Normal Streaming ]
            │
            │ (delta.prev_seq != client.last_seq)
            ▼
    [ Gap Detected ]
            │
            ├── 1. Start buffering incoming deltas in memory
            ├── 2. Send: {"type": "get_snapshot", "channel": "public.depth_l2"}
            ▼
    [ Receive Snapshot (seq = S) ]
            │
            ├── 3. Replace local order book image with Snapshot S
            ├── 4. Set client.last_seq = S
            ├── 5. Drop buffered deltas where delta.seq <= S
            ├── 6. Apply buffered deltas where delta.seq > S (verifying prev_seq)
            ▼
   [ Resume Live Streaming ]
```

---

## 7. Error Codes Reference

| Error Code | HTTP / Wire Equivalent | Description |
| :--- | :--- | :--- |
| `INVALID_QUANTITY` | 400 Bad Request | Order quantity is 0 or exceeds maximum allowable limit. |
| `INVALID_PRICE` | 400 Bad Request | Limit price is $\le 0$ or violates minimum tick size. |
| `DUPLICATE_ORDER_ID` | 409 Conflict | The explicit internal `order_id` is already assigned to an active order. |
| `DUPLICATE_CLIENT_ORDER_ID` | 409 Conflict | The `(user_id, client_order_id)` pair is already in use by an active order. |
| `UNKNOWN_ORDER` | 404 Not Found | The order ID specified in a cancel request does not exist or has already completed. |
| `UNAUTHORIZED` | 403 Forbidden | The caller `user_id` does not match the owner of the resting order. |
| `INVALID_SIDE` | 400 Bad Request | Side must be `"buy"` or `"sell"`. |
| `INVALID_ORDER_TYPE` | 400 Bad Request | Order type must be `"limit"` or `"market"`. |
| `MALFORMED_JSON` | 400 Bad Request | Request JSON is invalid or missing required envelope fields. |
| `RATE_LIMIT_EXCEEDED` | 429 Too Many Requests | Participant exceeded allocated request quota. |

---

## 8. Complete End-to-End Walkthrough Example

### Scenario: Limit Order Crossing Spread with Partial Fill and Residual Rest

1. **Book Initial State**:
   - Asks: `10 @ 100` (Order #1, User #99)
   - BBO: `Bid: (none), Ask: 100 [10]`

2. **Incoming Client Request** (User #101 submits `BUY 15 @ 100`):
   ```json
   {
     "req_id": "req-991",
     "type": "place_order",
     "payload": {
       "user_id": 101,
       "client_order_id": 7001,
       "side": "buy",
       "order_type": "limit",
       "price": 100,
       "quantity": 15
     }
   }
   ```

3. **Gateway Emits (Contiguous, Ordered Sequence)**:
   - **Step 3A (Private Order Ack to User #101)**:
     ```json
     {"req_id":"req-991","type":"order_accepted","seq":1,"payload":{"order_id":2,"user_id":101,"client_order_id":7001,"side":"buy","price":100,"quantity":15}}
     ```
   - **Step 3B (Private Execution to Taker User #101)**:
     ```json
     {"type":"execution","seq":2,"payload":{"order_id":2,"client_order_id":7001,"user_id":101,"side":"buy","role":"taker","match_price":100,"filled_qty":10,"remaining_qty":5,"order_status":"partially_filled"}}
     ```
   - **Step 3C (Private Execution to Maker User #99)**:
     ```json
     {"type":"execution","seq":3,"payload":{"order_id":1,"user_id":99,"side":"sell","role":"maker","match_price":100,"filled_qty":10,"remaining_qty":0,"order_status":"filled"}}
     ```
   - **Step 3D (Public Trade Feed)**:
     ```json
     {"type":"trade","channel":"public.trades","seq":4,"payload":{"price":100,"quantity":10,"aggressor_side":"buy"}}
     ```
   - **Step 3E (Public L2 Depth Update - Ask @ 100 Removed)**:
     ```json
     {"type":"depth_update","channel":"public.depth_l2","seq":5,"prev_seq":0,"payload":{"side":"sell","price":100,"new_qty":0}}
     ```
   - **Step 3F (Public L2 Depth Update - Residual Buy @ 100 Rests)**:
     ```json
     {"type":"depth_update","channel":"public.depth_l2","seq":6,"prev_seq":5,"payload":{"side":"buy","price":100,"new_qty":5}}
     ```
   - **Step 3G (Public BBO Update)**:
     ```json
     {"type":"bbo","channel":"public.bbo","seq":7,"payload":{"bid_price":100,"bid_qty":5,"ask_price":null,"ask_qty":0,"spread":null,"mid_price":null}}
     ```
