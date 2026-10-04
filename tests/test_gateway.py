#!/usr/bin/env python3
"""
test_gateway.py — Smoke-test client for Task 4.2 WebSocket Gateway.

Connects to ws://localhost:9001, runs a basic scenario:
  1. Subscribe to all public channels + get a depth snapshot.
  2. Place a resting sell order (maker).
  3. Place a buy order that crosses (taker).
  4. Verify the public trade is received.
  5. Cancel the residual buy (if any).

Usage:
  python test_gateway.py [host] [port]

Requires: websockets >= 11  (pip install websockets)
"""

import asyncio
import json
import sys
import websockets

HOST = sys.argv[1] if len(sys.argv) > 1 else "localhost"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9001
URI  = f"ws://{HOST}:{PORT}"

TIMEOUT = 5.0   # seconds per receive

async def recv_json(ws) -> dict:
    raw = await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)
    msg = json.loads(raw)
    print(f"  <-- {json.dumps(msg, separators=(',',':'))}")
    return msg

async def send_json(ws, msg: dict):
    text = json.dumps(msg)
    print(f"  --> {text}")
    await ws.send(text)

async def main():
    print(f"[test] connecting to {URI}")
    async with websockets.connect(URI) as ws:

        # ── 1. Subscribe to all public channels ──────────────────────────────
        await send_json(ws, {
            "req_id": "sub-1",
            "type": "subscribe",
            "payload": {"channels": ["public.bbo", "public.trades", "public.depth_l2"]}
        })
        # Expect: subscribed ack + depth_snapshot
        sub_ack = await recv_json(ws)
        assert sub_ack["type"] == "subscribed", f"Expected subscribed, got {sub_ack}"
        snap = await recv_json(ws)
        assert snap["type"] == "depth_snapshot", f"Expected depth_snapshot, got {snap}"
        print(f"  [ok] subscribed + snapshot (seq={snap['seq']})")

        # ── 2. Place a resting SELL limit order (maker) ───────────────────────
        import time
        t_base = int(time.time() * 1000) % 1000000
        clord_maker = t_base + 1
        clord_taker = t_base + 2

        await send_json(ws, {
            "req_id": "ord-maker",
            "type": "place_order",
            "payload": {
                "user_id": 99,
                "client_order_id": clord_maker,
                "side": "sell",
                "order_type": "limit",
                "price": 100,
                "quantity": 10
            }
        })
        order_ack = await recv_json(ws)
        assert order_ack["type"] == "order_accepted", f"Maker order not accepted: {order_ack}"
        maker_id = order_ack["payload"]["order_id"]
        print(f"  [ok] maker order accepted  order_id={maker_id}")

        # Collect the broadcast messages for this maker order (bbo + depth_update)
        maker_broadcasts = []
        for _ in range(2):
            maker_broadcasts.append(await recv_json(ws))

        mb_types = {m["type"] for m in maker_broadcasts}
        assert "depth_update" in mb_types, f"Missing depth_update: {maker_broadcasts}"
        assert "bbo" in mb_types, f"Missing bbo: {maker_broadcasts}"

        depth_msg = next(m for m in maker_broadcasts if m["type"] == "depth_update")
        assert depth_msg["payload"]["side"] == "sell"
        assert depth_msg["payload"]["price"] == 100
        assert depth_msg["payload"]["new_qty"] == 10
        print("  [ok] depth_update ask@100 qty=10")

        bbo_msg = next(m for m in maker_broadcasts if m["type"] == "bbo")
        print(f"  [ok] bbo: ask={bbo_msg['payload']['ask_price']}@{bbo_msg['payload']['ask_qty']}")

        # ── 3. Place a crossing BUY order (taker, 15 lots @ 100) ─────────────
        await send_json(ws, {
            "req_id": "ord-taker",
            "type": "place_order",
            "payload": {
                "user_id": 101,
                "client_order_id": clord_taker,
                "side": "buy",
                "order_type": "limit",
                "price": 100,
                "quantity": 15
            }
        })

        # Expect (in some order):
        #   - order_accepted (private, taker)
        #   - execution (private, taker fill)
        #   - trade (public)
        #   - depth_update ask@100 qty=0 (level removed)
        #   - depth_update bid@100 qty=5 (residual rests)
        #   - bbo update

        messages = []
        for _ in range(6):
            try:
                messages.append(await recv_json(ws))
            except asyncio.TimeoutError:
                break

        types = {m["type"] for m in messages}
        print(f"\n  received types: {types}")

        assert "order_accepted" in types, "Missing order_accepted for taker"
        assert "execution"      in types, "Missing execution (fill report)"
        assert "trade"          in types, "Missing public trade"
        assert "depth_update"   in types, "Missing depth_update"
        assert "bbo"            in types, "Missing bbo update"

        # Verify trade content (no user/order IDs!)
        trade = next(m for m in messages if m["type"] == "trade")
        assert "user_id"   not in trade.get("payload", {}), "PRIVACY VIOLATION: user_id in trade"
        assert "order_id"  not in trade.get("payload", {}), "PRIVACY VIOLATION: order_id in trade"
        assert trade["payload"]["price"]    == 100
        assert trade["payload"]["quantity"] == 10
        assert trade["payload"]["aggressor_side"] == "buy"
        print("  [ok] public trade: price=100 qty=10 aggressor=buy  (no user/order IDs)")

        # Verify execution report has required fields
        execution = next(m for m in messages if m["type"] == "execution")
        assert execution["payload"]["user_id"]   == 101
        assert execution["payload"]["role"]      == "taker"
        assert execution["payload"]["filled_qty"] == 10
        print(f"  [ok] private fill: user_id=101 role=taker filled=10"
              f" remaining={execution['payload']['remaining_qty']}")

        # ── 4. Cancel the residual buy (5 lots resting) ───────────────────────
        taker_order_id = next(
            m["payload"]["order_id"]
            for m in messages if m["type"] == "order_accepted"
        )
        await send_json(ws, {
            "req_id": "cxl-1",
            "type": "cancel_order",
            "payload": {"user_id": 101, "order_id": taker_order_id}
        })

        cancel_msgs = []
        for _ in range(3):
            try:
                cancel_msgs.append(await recv_json(ws))
            except asyncio.TimeoutError:
                break

        cancel_types = {m["type"] for m in cancel_msgs}
        assert "order_cancelled" in cancel_types, f"Missing order_cancelled: {cancel_types}"
        print("  [ok] cancel accepted — residual buy removed")

        # ── 5. Test malformed message handling ────────────────────────────────
        print("\n  Testing malformed message handling...")
        await ws.send("not valid json {{{")
        err = await recv_json(ws)
        assert err["status"] == "error"
        assert err["error"]["code"] == "MALFORMED_JSON"
        print("  [ok] malformed JSON handled gracefully (no crash)")

        print("\n[test] ALL CHECKS PASSED [OK]")

if __name__ == "__main__":
    asyncio.run(main())
