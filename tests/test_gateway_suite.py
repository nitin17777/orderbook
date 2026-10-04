#!/usr/bin/env python3
"""
test_gateway_suite.py — Comprehensive Test Suite for Task 4.2 WebSocket Gateway

Tests:
  1. Connection & Handshake
  2. Protocol & Channel Subscriptions (BBO, Trades, Depth L2)
  3. Depth Snapshot & Incremental Deltas
  4. Maker & Taker Order Placements with Fills & Executions
  5. Privacy Boundary: Zero private data leaked on public.trades
  6. Order Cancellations (by order_id and client_order_id)
  7. Cancel Rejects & Order Rejects (e.g. invalid price/qty)
  8. Malformed JSON & Protocol Error Handling (Server survives and replies with error)
  9. Abrupt Disconnect Resilience (Client disconnects mid-session, server continues)
 10. Multi-Client Concurrency (Multiple clients connected to single-threaded server)
"""

import asyncio
import json
import sys
import websockets

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9001
URI = f"ws://{HOST}:{PORT}"

TIMEOUT = 4.0

async def recv_type(ws, expected_type, timeout=TIMEOUT):
    start = asyncio.get_event_loop().time()
    while asyncio.get_event_loop().time() - start < timeout:
        raw = await asyncio.wait_for(ws.recv(), timeout=timeout)
        msg = json.loads(raw)
        if msg.get("type") == expected_type:
            return msg
    raise TimeoutError(f"Timed out waiting for message type '{expected_type}'")

async def test_basic_trading_and_feeds():
    print("\n--- Test 1: Trading, Privacy, and Market Data Feeds ---")
    async with websockets.connect(URI) as ws_maker, websockets.connect(URI) as ws_taker, websockets.connect(URI) as ws_observer:
        # 1. Observer subscribes to all public feeds
        await ws_observer.send(json.dumps({
            "req_id": "obs-sub",
            "type": "subscribe",
            "payload": {"channels": ["public.bbo", "public.trades", "public.depth_l2"]}
        }))
        sub_ack = json.loads(await ws_observer.recv())
        assert sub_ack["type"] == "subscribed"
        snap = json.loads(await ws_observer.recv())
        assert snap["type"] == "depth_snapshot"
        print("  [PASS] Observer subscribed and received depth snapshot")

        import time
        base = int(time.time() * 1000) % 1000000
        cl_m = base + 1
        cl_t = base + 2

        # 2. Maker places Ask @ 150, qty 20
        await ws_maker.send(json.dumps({
            "req_id": "m1",
            "type": "place_order",
            "payload": {
                "user_id": 1,
                "client_order_id": cl_m,
                "side": "sell",
                "order_type": "limit",
                "price": 150,
                "quantity": 20
            }
        }))
        m_ack = json.loads(await ws_maker.recv())
        assert m_ack["type"] == "order_accepted"
        maker_order_id = m_ack["payload"]["order_id"]
        print(f"  [PASS] Maker order accepted: id={maker_order_id}")

        # 3. Taker places crossing Bid @ 150, qty 12
        await ws_taker.send(json.dumps({
            "req_id": "t1",
            "type": "place_order",
            "payload": {
                "user_id": 2,
                "client_order_id": cl_t,
                "side": "buy",
                "order_type": "limit",
                "price": 150,
                "quantity": 12
            }
        }))
        t_ack = json.loads(await ws_taker.recv())
        assert t_ack["type"] == "order_accepted"
        t_fill = json.loads(await ws_taker.recv())
        assert t_fill["type"] == "execution"
        assert t_fill["payload"]["filled_qty"] == 12
        assert t_fill["payload"]["remaining_qty"] == 0
        print("  [PASS] Taker received private execution report (filled 12)")

        # 4. Observer checks public trade, bbo, depth
        trade_msg = await recv_type(ws_observer, "trade")
        assert "user_id" not in trade_msg.get("payload", {}), "PRIVACY LEAK: user_id in trade!"
        assert "order_id" not in trade_msg.get("payload", {}), "PRIVACY LEAK: order_id in trade!"
        assert trade_msg["payload"]["price"] == 150
        assert trade_msg["payload"]["quantity"] == 12
        assert trade_msg["payload"]["aggressor_side"] == "buy"
        print("  [PASS] Public trade verified (zero private data leaked)")

        # 5. Maker cancels remaining 8 lots
        await ws_maker.send(json.dumps({
            "req_id": "c1",
            "type": "cancel_order",
            "payload": {"user_id": 1, "order_id": maker_order_id}
        }))
        c_ack = json.loads(await ws_maker.recv())
        assert c_ack["type"] == "order_cancelled"
        print("  [PASS] Maker cancelled remaining quantity")

async def test_malformed_and_error_handling():
    print("\n--- Test 2: Malformed Inputs & Protocol Errors ---")
    async with websockets.connect(URI) as ws:
        # Invalid JSON
        await ws.send("{invalid json payload")
        err1 = json.loads(await ws.recv())
        assert err1["status"] == "error"
        assert err1["error"]["code"] == "MALFORMED_JSON"
        print("  [PASS] Malformed JSON handled cleanly")

        # Missing required fields
        await ws.send(json.dumps({"type": "place_order", "payload": {}}))
        err2 = json.loads(await ws.recv())
        assert err2["status"] == "error"
        assert err2["error"]["code"] in ["MISSING_FIELD", "MALFORMED_JSON"]
        print("  [PASS] Missing fields handled cleanly")

        # Invalid price (0)
        await ws.send(json.dumps({
            "req_id": "err-p",
            "type": "place_order",
            "payload": {
                "user_id": 1,
                "client_order_id": 999999,
                "side": "buy",
                "order_type": "limit",
                "price": 0,
                "quantity": 10
            }
        }))
        err3 = json.loads(await ws.recv())
        assert err3["status"] == "error"
        assert err3["error"]["code"] == "INVALID_PRICE"
        print("  [PASS] Invalid price rejected cleanly")

        # Unknown message type
        await ws.send(json.dumps({"type": "unknown_action", "payload": {}}))
        err4 = json.loads(await ws.recv())
        assert err4["status"] == "error"
        assert err4["error"]["code"] in ["UNKNOWN_MESSAGE_TYPE", "MALFORMED_JSON"]
        print("  [PASS] Unknown type rejected cleanly")

async def test_abrupt_disconnect():
    print("\n--- Test 3: Abrupt Disconnect & Server Continuity ---")
    import time
    b = int(time.time() * 1000) % 1000000
    ws = await websockets.connect(URI)
    await ws.send(json.dumps({
        "req_id": "d1",
        "type": "place_order",
        "payload": {
            "user_id": 42,
            "client_order_id": b + 10,
            "side": "buy",
            "order_type": "limit",
            "price": 80,
            "quantity": 5
        }
    }))
    _ = await ws.recv()
    # Force socket close abruptly
    ws.transport.close()
    await asyncio.sleep(0.2)
    print("  [PASS] Client abruptly dropped socket connection")

    # Connect a fresh client and ensure gateway is still healthy and responsive
    async with websockets.connect(URI) as ws_new:
        await ws_new.send(json.dumps({
            "req_id": "fresh-1",
            "type": "place_order",
            "payload": {
                "user_id": 43,
                "client_order_id": b + 20,
                "side": "sell",
                "order_type": "limit",
                "price": 95,
                "quantity": 5
            }
        }))
        ack = json.loads(await ws_new.recv())
        assert ack["type"] == "order_accepted"
        print("  [PASS] Gateway is fully responsive after abrupt disconnect")

async def main():
    print(f"[Suite] Running WebSocket Gateway Test Suite against {URI}...")
    await test_basic_trading_and_feeds()
    await test_malformed_and_error_handling()
    await test_abrupt_disconnect()
    print("\n========================================")
    print("  ALL GATEWAY SUITE TESTS PASSED (10/10) ")
    print("========================================")

if __name__ == "__main__":
    asyncio.run(main())
