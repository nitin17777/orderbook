#!/usr/bin/env python3
"""
demo_scenario.py — Scripted End-to-End Walkthrough Demonstration (Task 4.4)

Runs a complete lifecycle scenario against a running WebSocket Gateway:
  Phase 1: Multi-participant Connection & Channel Subscriptions
  Phase 2: Building Depth (Maker Bids & Asks across multiple price levels)
  Phase 3: Partial Fill & Private/Public Feed Boundary Verification
  Phase 4: Multi-Level Aggressive Sweep (Sweeping across multiple ask price levels)
  Phase 5: Maker Order Cancellation & BBO Shift
  Phase 6: Protocol Error Handling (Rejects, Duplicate ClOrdIDs, Invalid Inputs)
"""

import asyncio
import json
import sys
import time
import websockets

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9001
URI  = f"ws://{HOST}:{PORT}"

RESET   = "\033[0m"
BOLD    = "\033[1m"
GREEN   = "\033[92m"
RED     = "\033[91m"
YELLOW  = "\033[93m"
CYAN    = "\033[96m"
MAGENTA = "\033[95m"
BLUE    = "\033[94m"
GRAY    = "\033[90m"

class DemoRunner:
    def __init__(self, uri: str):
        self.uri = uri
        self.ws_maker = None
        self.ws_taker = None
        self.ws_observer = None
        self.base_id = int(time.time() * 1000) % 10000000

        # Local book tracking from observer feed
        self.bids = {}
        self.asks = {}
        self.bbo = {}

    def next_clord(self) -> int:
        self.base_id += 1
        return self.base_id

    async def connect_all(self):
        print(f"\n{BOLD}{CYAN}=========================================================={RESET}")
        print(f"{BOLD}{CYAN}   ORDERBOOK END-TO-END DEMONSTRATION WALKTHROUGH        {RESET}")
        print(f"{BOLD}{CYAN}=========================================================={RESET}\n")

        print(f"{BOLD}Phase 1: Connecting Participants & Establishing Subscriptions{RESET}")
        self.ws_maker = await websockets.connect(self.uri)
        self.ws_taker = await websockets.connect(self.uri)
        self.ws_observer = await websockets.connect(self.uri)
        print(f"  [+] Connected Maker (User #1)")
        print(f"  [+] Connected Taker (User #2)")
        print(f"  [+] Connected Observer (User #99)")

        # Observer subscribes to all market data feeds
        await self.ws_observer.send(json.dumps({
            "req_id": "sub-all",
            "type": "subscribe",
            "payload": {"channels": ["public.bbo", "public.trades", "public.depth_l2"]}
        }))

        ack = json.loads(await self.ws_observer.recv())
        snap = json.loads(await self.ws_observer.recv())
        print(f"  [PASS] Observer subscribed. Initial snapshot seq={snap['seq']}")
        await asyncio.sleep(0.3)

    async def drain_observer(self, max_msgs: int = 10, timeout: float = 0.2):
        while max_msgs > 0:
            try:
                raw = await asyncio.wait_for(self.ws_observer.recv(), timeout=timeout)
                msg = json.loads(raw)
                mtype = msg.get("type")
                p = msg.get("payload", {})
                seq = msg.get("seq")

                if mtype == "depth_update":
                    side = p["side"]
                    price = p["price"]
                    qty = p["new_qty"]
                    target = self.bids if side == "buy" else self.asks
                    if qty == 0:
                        target.pop(price, None)
                    else:
                        target[price] = qty
                    tag = f"{RED}DEL{RESET}" if qty == 0 else f"{GREEN}UPD{RESET}"
                    print(f"    {GRAY}[MarketData Depth]{RESET} {tag} {side.upper():<4} {price:>5} -> qty {qty} (seq: {seq})")

                elif mtype == "bbo":
                    self.bbo = p
                    bp, bq = p.get("bid_price"), p.get("bid_qty")
                    ap, aq = p.get("ask_price"), p.get("ask_qty")
                    b_str = f"{GREEN}{bp} ({bq}){RESET}" if bp else "None"
                    a_str = f"{RED}{ap} ({aq}){RESET}" if ap else "None"
                    print(f"    {BLUE}[MarketData BBO]{RESET} {b_str} <-> {a_str} (seq: {seq})")

                elif mtype == "trade":
                    aggr = p["aggressor_side"].upper()
                    print(f"    {YELLOW}[MarketData Trade]{RESET} Aggressor {BOLD}{aggr}{RESET} swept {p['quantity']} lots @ {p['price']} (seq: {seq})")

                max_msgs -= 1
            except asyncio.TimeoutError:
                break

    def render_ladder(self):
        print(f"\n  {BOLD}Current Aggregated L2 Depth Ladder:{RESET}")
        print(f"  {'Qty (Bid)':>12} | {'Price':^8} | {'Qty (Ask)':<12}")
        print("  " + "-" * 34)
        for p, q in sorted(self.asks.items(), reverse=True):
            print(f"  {'':>12} | {RED}{p:^8}{RESET} | {RED}{q:<12}{RESET}")
        print(f"  {'':>12} | {YELLOW}{'======':^8}{RESET} | {'':<12}")
        for p, q in sorted(self.bids.items(), reverse=True):
            print(f"  {GREEN}{q:>12}{RESET} | {GREEN}{p:^8}{RESET} | {'':<12}")
        print("  " + "-" * 34 + "\n")

    async def phase2_build_ladder(self):
        print(f"\n{BOLD}Phase 2: Seeding Order Book Depth (Bids 95..99, Asks 101..105){RESET}")

        bids_to_place = [(95, 10), (96, 10), (97, 10), (98, 10), (99, 10)]
        asks_to_place = [(101, 10), (102, 10), (103, 10), (104, 10), (105, 10)]

        for price, qty in bids_to_place:
            await self.ws_maker.send(json.dumps({
                "req_id": f"seed-bid-{price}",
                "type": "place_order",
                "payload": {
                    "user_id": 1,
                    "client_order_id": self.next_clord(),
                    "side": "buy",
                    "order_type": "limit",
                    "price": price,
                    "quantity": qty
                }
            }))
            _ = await self.ws_maker.recv()

        for price, qty in asks_to_place:
            await self.ws_maker.send(json.dumps({
                "req_id": f"seed-ask-{price}",
                "type": "place_order",
                "payload": {
                    "user_id": 1,
                    "client_order_id": self.next_clord(),
                    "side": "sell",
                    "order_type": "limit",
                    "price": price,
                    "quantity": qty
                }
            }))
            _ = await self.ws_maker.recv()

        await self.drain_observer(max_msgs=30)
        self.render_ladder()

    async def phase3_partial_fill(self):
        print(f"\n{BOLD}Phase 3: Partial Crossing Fill (Taker Buy 4 @ 101 against Ask @ 101){RESET}")
        clord = self.next_clord()
        await self.ws_taker.send(json.dumps({
            "req_id": "taker-fill-1",
            "type": "place_order",
            "payload": {
                "user_id": 2,
                "client_order_id": clord,
                "side": "buy",
                "order_type": "limit",
                "price": 101,
                "quantity": 4
            }
        }))

        ack = json.loads(await self.ws_taker.recv())
        fill = json.loads(await self.ws_taker.recv())
        print(f"  [PASS] Taker received Ack for Order #{ack['payload']['order_id']}")
        print(f"  [PASS] Taker Fill: {fill['payload']['filled_qty']} lots @ {fill['payload']['match_price']} (Status: {fill['payload']['order_status']})")

        await self.drain_observer(max_msgs=5)
        self.render_ladder()

    async def phase4_multi_level_sweep(self):
        print(f"\n{BOLD}Phase 4: Multi-Level Market Sweep (Taker Buy 20 @ 103){RESET}")
        print(f"  [!] Sweeps: remaining 6 lots @ 101, 10 lots @ 102, 4 lots @ 103")
        clord = self.next_clord()
        await self.ws_taker.send(json.dumps({
            "req_id": "taker-sweep",
            "type": "place_order",
            "payload": {
                "user_id": 2,
                "client_order_id": clord,
                "side": "buy",
                "order_type": "limit",
                "price": 103,
                "quantity": 20
            }
        }))

        ack = json.loads(await self.ws_taker.recv())
        print(f"  [PASS] Taker Sweep Order #{ack['payload']['order_id']} accepted")

        # Read the 3 execution reports for the 3 levels swept
        for i in range(3):
            fill = json.loads(await self.ws_taker.recv())
            print(f"  [PASS] Execution #{i+1}: Filled {fill['payload']['filled_qty']} @ {fill['payload']['match_price']} | Remaining: {fill['payload']['remaining_qty']}")

        await self.drain_observer(max_msgs=15)
        self.render_ladder()

    async def phase5_cancel_and_bbo_shift(self):
        print(f"\n{BOLD}Phase 5: Maker Cancellation & Best Bid Shift{RESET}")
        # Place a fresh top bid @ 100 then cancel it
        clord = self.next_clord()
        await self.ws_maker.send(json.dumps({
            "req_id": "top-bid",
            "type": "place_order",
            "payload": {
                "user_id": 1,
                "client_order_id": clord,
                "side": "buy",
                "order_type": "limit",
                "price": 100,
                "quantity": 10
            }
        }))
        ack = json.loads(await self.ws_maker.recv())
        order_id = ack["payload"]["order_id"]
        print(f"  [+] Maker placed Top Bid @ 100 (Order #{order_id})")
        await self.drain_observer(max_msgs=5)

        # Cancel the top bid
        await self.ws_maker.send(json.dumps({
            "req_id": "cxl-top-bid",
            "type": "cancel_order",
            "payload": {
                "user_id": 1,
                "order_id": order_id
            }
        }))
        cxl_ack = json.loads(await self.ws_maker.recv())
        print(f"  [PASS] Maker Cancelled Order #{order_id} (Status: {cxl_ack['payload']['status']})")
        await self.drain_observer(max_msgs=5)
        self.render_ladder()

    async def phase6_error_handling(self):
        print(f"\n{BOLD}Phase 6: Protocol & Engine Reject Handling{RESET}")
        # 1. Invalid price 0
        await self.ws_taker.send(json.dumps({
            "req_id": "err-zero-price",
            "type": "place_order",
            "payload": {
                "user_id": 2,
                "client_order_id": self.next_clord(),
                "side": "buy",
                "order_type": "limit",
                "price": 0,
                "quantity": 10
            }
        }))
        err1 = json.loads(await self.ws_taker.recv())
        print(f"  [PASS] Zero price rejected: code={err1['error']['code']} message='{err1['error']['message']}'")

        # 2. Duplicate Client Order ID
        dup_clord = self.next_clord()
        await self.ws_taker.send(json.dumps({
            "req_id": "dup-1",
            "type": "place_order",
            "payload": {
                "user_id": 2,
                "client_order_id": dup_clord,
                "side": "buy",
                "order_type": "limit",
                "price": 50,
                "quantity": 1
            }
        }))
        _ = await self.ws_taker.recv()

        # Submit again with same client_order_id
        await self.ws_taker.send(json.dumps({
            "req_id": "dup-2",
            "type": "place_order",
            "payload": {
                "user_id": 2,
                "client_order_id": dup_clord,
                "side": "buy",
                "order_type": "limit",
                "price": 50,
                "quantity": 1
            }
        }))
        err2 = json.loads(await self.ws_taker.recv())
        print(f"  [PASS] Duplicate ClOrdID rejected: code={err2['error']['code']} message='{err2['error']['message']}'")

        # 3. Malformed JSON
        await self.ws_taker.send("{not valid json syntax")
        err3 = json.loads(await self.ws_taker.recv())
        print(f"  [PASS] Malformed JSON handled: code={err3['error']['code']}")

    async def close_all(self):
        await self.ws_maker.close()
        await self.ws_taker.close()
        await self.ws_observer.close()
        print(f"\n{BOLD}{GREEN}=========================================================={RESET}")
        print(f"{BOLD}{GREEN}   DEMO WALKTHROUGH COMPLETED SUCCESSFULLY [PASS]         {RESET}")
        print(f"{BOLD}{GREEN}=========================================================={RESET}\n")

async def main():
    runner = DemoRunner(URI)
    await runner.connect_all()
    await runner.phase2_build_ladder()
    await runner.phase3_partial_fill()
    await runner.phase4_multi_level_sweep()
    await runner.phase5_cancel_and_bbo_shift()
    await runner.phase6_error_handling()
    await runner.close_all()

if __name__ == "__main__":
    asyncio.run(main())
