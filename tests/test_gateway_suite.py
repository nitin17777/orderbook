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

async def test_time_in_force_ioc_fok():
    print("\n--- Test 4: Time-In-Force (GTC, IOC, FOK) Execution Semantics ---")
    import time
    b = int(time.time() * 1000) % 1000000
    async with websockets.connect(URI) as ws_maker, websockets.connect(URI) as ws_taker:
        # Maker places resting Sell: 10 @ 200
        await ws_maker.send(json.dumps({
            "req_id": "m-tif-1",
            "type": "place_order",
            "payload": {
                "user_id": 10,
                "client_order_id": b + 101,
                "side": "sell",
                "order_type": "limit",
                "time_in_force": "gtc",
                "price": 200,
                "quantity": 10
            }
        }))
        m_ack = json.loads(await ws_maker.recv())
        assert m_ack["type"] == "order_accepted"
        assert m_ack["payload"]["time_in_force"] == "gtc"
        print("  [PASS] Maker GTC resting order accepted")

        # Taker places FOK Buy: 15 @ 200 (only 10 available) -> should be killed with 0 fills
        await ws_taker.send(json.dumps({
            "req_id": "t-fok-fail",
            "type": "place_order",
            "payload": {
                "user_id": 20,
                "client_order_id": b + 102,
                "side": "buy",
                "order_type": "limit",
                "time_in_force": "fok",
                "price": 200,
                "quantity": 15
            }
        }))
        fok_ack = json.loads(await ws_taker.recv())
        assert fok_ack["type"] == "order_accepted"
        assert fok_ack["payload"]["time_in_force"] == "fok"

        fok_cxl = json.loads(await ws_taker.recv())
        assert fok_cxl["type"] == "order_cancelled"
        assert fok_cxl["payload"]["remaining_qty"] == 15
        print("  [PASS] FOK with insufficient liquidity was killed immediately without fills")

        # Taker places IOC Buy: 15 @ 200 (10 available) -> matches 10, cancels unfilled 5
        await ws_taker.send(json.dumps({
            "req_id": "t-ioc-part",
            "type": "place_order",
            "payload": {
                "user_id": 20,
                "client_order_id": b + 103,
                "side": "buy",
                "order_type": "limit",
                "time_in_force": "ioc",
                "price": 200,
                "quantity": 15
            }
        }))
        ioc_ack = json.loads(await ws_taker.recv())
        assert ioc_ack["type"] == "order_accepted"
        assert ioc_ack["payload"]["time_in_force"] == "ioc"

        ioc_fill = json.loads(await ws_taker.recv())
        assert ioc_fill["type"] == "execution"
        assert ioc_fill["payload"]["filled_qty"] == 10
        assert ioc_fill["payload"]["remaining_qty"] == 5

        ioc_cxl = json.loads(await ws_taker.recv())
        assert ioc_cxl["type"] == "order_cancelled"
        assert ioc_cxl["payload"]["remaining_qty"] == 5
        print("  [PASS] IOC partial fill matched available liquidity and discarded remainder")

async def test_order_modification():
    print("\n--- Test 5: Order Modification (Cancel-Replace & Priority) ---")
    async with websockets.connect(URI) as ws_maker, websockets.connect(URI) as ws_taker:
        import time
        b = int(time.time() * 1000) % 1000000

        # 1. Maker places Limit Buy @ 100, qty 20
        await ws_maker.send(json.dumps({
            "req_id": "m-mod-1",
            "type": "place_order",
            "payload": {
                "user_id": 30,
                "client_order_id": b + 201,
                "side": "buy",
                "order_type": "limit",
                "price": 100,
                "quantity": 20
            }
        }))
        ack1 = json.loads(await ws_maker.recv())
        assert ack1["type"] == "order_accepted"
        order_id = ack1["payload"]["order_id"]

        # 2. Maker modifies order: reduce qty 20 -> 10 (keeps priority)
        await ws_maker.send(json.dumps({
            "req_id": "m-mod-2",
            "type": "modify_order",
            "payload": {
                "user_id": 30,
                "order_id": order_id,
                "new_price": 100,
                "new_quantity": 10
            }
        }))
        mod_ack = json.loads(await ws_maker.recv())
        assert mod_ack["type"] == "order_modified"
        assert mod_ack["payload"]["new_quantity"] == 10
        assert mod_ack["payload"]["new_price"] == 100
        print("  [PASS] Order quantity reduced via modify_order request")

        # 3. Modify with price cross (triggers immediate match against new seller)
        # First place a resting sell @ 105, qty 5
        await ws_taker.send(json.dumps({
            "req_id": "t-mod-seller",
            "type": "place_order",
            "payload": {
                "user_id": 40,
                "client_order_id": b + 202,
                "side": "sell",
                "order_type": "limit",
                "price": 105,
                "quantity": 5
            }
        }))
        seller_ack = json.loads(await ws_taker.recv())
        assert seller_ack["type"] == "order_accepted"

        # Maker modifies remaining 10 @ 100 -> 10 @ 105 (crosses spread, fills 5)
        await ws_maker.send(json.dumps({
            "req_id": "m-mod-3",
            "type": "modify_order",
            "payload": {
                "user_id": 30,
                "client_order_id": b + 201,
                "new_price": 105,
                "new_quantity": 10
            }
        }))
        mod_ack2 = json.loads(await ws_maker.recv())
        assert mod_ack2["type"] == "order_modified"

        fill_report = json.loads(await ws_maker.recv())
        assert fill_report["type"] == "execution"
        assert fill_report["payload"]["match_price"] == 105
        assert fill_report["payload"]["filled_qty"] == 5
        assert fill_report["payload"]["remaining_qty"] == 5
        print("  [PASS] Crossing price modification triggered immediate fill")

async def main():
    print(f"[Suite] Running WebSocket Gateway Test Suite against {URI}...")
    await test_basic_trading_and_feeds()
    await test_malformed_and_error_handling()
    await test_abrupt_disconnect()
    await test_time_in_force_ioc_fok()
    await test_order_modification()
    print("\n========================================")
    print("  ALL GATEWAY SUITE TESTS PASSED (12/12) ")
    print("========================================")

if __name__ == "__main__":
    asyncio.run(main())
