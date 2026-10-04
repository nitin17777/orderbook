#!/usr/bin/env python3
"""
client.py — Reference WebSocket Client for OrderBook Exchange (Task 4.4)

An interactive and scripted CLI client that connects to the WebSocket Gateway,
submits orders, cancels orders, subscribes to public market data streams,
and pretty-prints private execution reports and public market data.

Usage:
  python client/client.py [--host HOST] [--port PORT] [--user USER_ID]

Commands (Interactive Mode):
  buy <qty> @ <price>     - Place a limit Buy order
  sell <qty> @ <price>    - Place a limit Sell order
  cancel <order_id>       - Cancel an order by order ID
  sub [channels...]       - Subscribe to public channels (bbo, trades, depth_l2)
  snapshot                - Request immediate L2 depth snapshot
  help                    - Show available commands
  quit / exit             - Disconnect and exit
"""

import asyncio
import json
import sys
import time
import argparse
import websockets

RESET   = "\033[0m"
BOLD    = "\033[1m"
GREEN   = "\033[92m"
RED     = "\033[91m"
YELLOW  = "\033[93m"
CYAN    = "\033[96m"
MAGENTA = "\033[95m"
BLUE    = "\033[94m"
GRAY    = "\033[90m"

class OrderBookClient:
    def __init__(self, uri: str, user_id: int):
        self.uri = uri
        self.user_id = user_id
        self.ws = None
        self.req_counter = 0
        self.clord_counter = int(time.time() * 1000) % 10000000
        self.running = True

        # Local book view maintained from depth feeds
        self.bids = {}  # price -> qty
        self.asks = {}  # price -> qty
        self.bbo = {"bid_price": None, "bid_qty": 0, "ask_price": None, "ask_qty": 0}

    def next_req_id(self, prefix: str = "req") -> str:
        self.req_counter += 1
        return f"{prefix}-{self.req_counter}"

    def next_client_order_id(self) -> int:
        self.clord_counter += 1
        return self.clord_counter

    async def connect(self):
        print(f"{CYAN}{BOLD}[Client]{RESET} Connecting to {self.uri} as User #{self.user_id}...")
        self.ws = await websockets.connect(self.uri)
        print(f"{GREEN}{BOLD}[Client]{RESET} Connected successfully!\n")

    def pretty_print_msg(self, msg: dict):
        mtype = msg.get("type", "unknown")
        chan  = msg.get("channel", "")
        seq   = msg.get("seq", "-")
        p     = msg.get("payload", {})

        if mtype == "subscribed":
            channels = p.get("channels", [])
            print(f"{CYAN}[SUB ACK]{RESET} Subscribed to channels: {BOLD}{', '.join(channels)}{RESET}")

        elif mtype == "order_accepted":
            print(f"{GREEN}[ORDER ACK]{RESET} Order #{BOLD}{p.get('order_id')}{RESET} accepted | "
                  f"Side: {BOLD}{p.get('side').upper()}{RESET} | "
                  f"Qty: {BOLD}{p.get('quantity')}{RESET} @ Price: {BOLD}{p.get('price')}{RESET} | "
                  f"ClOrdID: {p.get('client_order_id')} (seq: {seq})")

        elif mtype == "order_rejected":
            err = msg.get("error", {})
            print(f"{RED}[ORDER REJECT]{RESET} Code: {BOLD}{err.get('code')}{RESET} | "
                  f"Reason: {err.get('message')} | ClOrdID: {p.get('client_order_id')} (seq: {seq})")

        elif mtype == "execution":
            role = p.get("role", "").upper()
            status = p.get("order_status", "")
            print(f"{YELLOW}{BOLD}[FILL / EXECUTION]{RESET} "
                  f"Role: {BOLD}{role}{RESET} | Order #{p.get('order_id')} | "
                  f"Side: {p.get('side').upper()} | "
                  f"Filled: {BOLD}{p.get('filled_qty')}{RESET} @ Price: {BOLD}{p.get('match_price')}{RESET} | "
                  f"Remaining: {p.get('remaining_qty')} ({status}) (seq: {seq})")

        elif mtype == "order_cancelled":
            print(f"{MAGENTA}[CANCEL ACK]{RESET} Order #{BOLD}{p.get('order_id')}{RESET} cancelled | "
                  f"Remaining qty removed: {p.get('remaining_qty')} (seq: {seq})")

        elif mtype == "cancel_rejected":
            err = msg.get("error", {})
            print(f"{RED}[CANCEL REJECT]{RESET} Code: {BOLD}{err.get('code')}{RESET} | "
                  f"Reason: {err.get('message')} | Order #{p.get('order_id')} (seq: {seq})")

        elif mtype == "trade":
            aggr = p.get("aggressor_side", "").upper()
            aggr_color = GREEN if aggr == "BUY" else RED
            print(f"{BOLD}[PUBLIC TRADE]{RESET} {aggr_color}{BOLD}{aggr}{RESET} swept "
                  f"Qty: {BOLD}{p.get('quantity')}{RESET} @ Price: {BOLD}{p.get('price')}{RESET} (seq: {seq})")

        elif mtype == "bbo":
            bp = p.get("bid_price")
            bq = p.get("bid_qty")
            ap = p.get("ask_price")
            aq = p.get("ask_qty")
            self.bbo = p
            bid_str = f"{GREEN}{bp} ({bq}){RESET}" if bp is not None else f"{GRAY}None{RESET}"
            ask_str = f"{RED}{ap} ({aq}){RESET}" if ap is not None else f"{GRAY}None{RESET}"
            spread  = p.get("spread")
            spread_str = f"| Spread: {spread}" if spread is not None else ""
            print(f"{BLUE}[BBO UPDATE]{RESET} Best Bid: {bid_str} <-> Best Ask: {ask_str} {spread_str} (seq: {seq})")

        elif mtype == "depth_snapshot":
            self.bids = {lvl["price"]: lvl["qty"] for lvl in p.get("bids", [])}
            self.asks = {lvl["price"]: lvl["qty"] for lvl in p.get("asks", [])}
            print(f"{CYAN}[DEPTH SNAPSHOT]{RESET} Loaded {len(self.bids)} bids, {len(self.asks)} asks (seq: {seq})")
            self.render_depth_ladder()

        elif mtype == "depth_update":
            side = p.get("side")
            price = p.get("price")
            new_qty = p.get("new_qty")
            target = self.bids if side == "buy" else self.asks
            if new_qty == 0:
                target.pop(price, None)
            else:
                target[price] = new_qty
            delta_tag = f"{RED}DEL{RESET}" if new_qty == 0 else f"{GREEN}UPD{RESET}"
            print(f"{GRAY}[DEPTH DELTA]{RESET} {delta_tag} {side.upper()} @ {price} -> qty={new_qty} (seq: {seq})")

        elif mtype == "error":
            err = msg.get("error", {})
            print(f"{RED}{BOLD}[SYSTEM ERROR]{RESET} [{err.get('code')}] {err.get('message')}")

        else:
            print(f"{GRAY}[RAW]{RESET} {json.dumps(msg)}")

    def render_depth_ladder(self, levels: int = 5):
        print(f"\n{BOLD}{'--- L2 BOOK LADDER ---':^34}{RESET}")
        print(f"{'Qty':>12} | {'Price':^8} | {'Qty':<12}")
        print("-" * 34)

        sorted_asks = sorted(self.asks.items(), key=lambda x: x[0])[:levels]
        sorted_bids = sorted(self.bids.items(), key=lambda x: x[0], reverse=True)[:levels]

        # Print asks (top of book closest to middle)
        for price, qty in reversed(sorted_asks):
            print(f"{'':>12} | {RED}{price:^8}{RESET} | {RED}{qty:<12}{RESET}")

        print(f"{'':>12} | {YELLOW}{'======':^8}{RESET} | {'':<12}")

        # Print bids
        for price, qty in sorted_bids:
            print(f"{GREEN}{qty:>12}{RESET} | {GREEN}{price:^8}{RESET} | {'':<12}")
        print("-" * 34 + "\n")

    async def listen_loop(self):
        try:
            async for raw in self.ws:
                try:
                    msg = json.loads(raw)
                    self.pretty_print_msg(msg)
                except Exception as e:
                    print(f"{RED}[Error parsing JSON]{RESET} {e}: {raw}")
        except websockets.ConnectionClosed:
            if self.running:
                print(f"{YELLOW}[Client] Server disconnected.{RESET}")
        except asyncio.CancelledError:
            pass

    async def send_json(self, msg: dict):
        await self.ws.send(json.dumps(msg))

    async def subscribe(self, channels: list = None):
        if channels is None:
            channels = ["public.bbo", "public.trades", "public.depth_l2"]
        await self.send_json({
            "req_id": self.next_req_id("sub"),
            "type": "subscribe",
            "payload": {"channels": channels}
        })

    async def place_order(self, side: str, price: int, quantity: int, order_type: str = "limit"):
        clord_id = self.next_client_order_id()
        await self.send_json({
            "req_id": self.next_req_id("ord"),
            "type": "place_order",
            "payload": {
                "user_id": self.user_id,
                "client_order_id": clord_id,
                "side": side.lower(),
                "order_type": order_type.lower(),
                "price": price,
                "quantity": quantity
            }
        })

    async def cancel_order(self, order_id: int):
        await self.send_json({
            "req_id": self.next_req_id("cxl"),
            "type": "cancel_order",
            "payload": {
                "user_id": self.user_id,
                "order_id": order_id
            }
        })

    async def request_snapshot(self):
        await self.send_json({
            "req_id": self.next_req_id("snap"),
            "type": "get_snapshot",
            "payload": {"channel": "public.depth_l2"}
        })

    async def interactive_shell(self):
        loop = asyncio.get_event_loop()
        print(f"{CYAN}Type 'help' for commands, 'quit' to exit.{RESET}")
        while self.running:
            try:
                line = await loop.run_in_executor(None, input, f"{BOLD}orderbook> {RESET}")
            except (EOFError, KeyboardInterrupt):
                break

            cmd = line.strip().split()
            if not cmd:
                continue

            action = cmd[0].lower()
            if action in ("quit", "exit", "q"):
                self.running = False
                break

            elif action == "help":
                print("\nCommands:")
                print("  buy <qty> @ <price>       Place limit Buy order")
                print("  sell <qty> @ <price>      Place limit Sell order")
                print("  cancel <order_id>         Cancel an active order")
                print("  sub                       Subscribe to all public streams")
                print("  ladder                    Display current local depth ladder")
                print("  snapshot                  Request L2 depth snapshot")
                print("  quit                      Exit client\n")

            elif action in ("buy", "sell"):
                try:
                    # buy 10 @ 150  or  buy 10 150
                    qty = int(cmd[1])
                    if len(cmd) >= 4 and cmd[2] == "@":
                        price = int(cmd[3])
                    elif len(cmd) >= 3:
                        price = int(cmd[2])
                    else:
                        print(f"{RED}Usage: {action} <qty> @ <price>{RESET}")
                        continue
                    await self.place_order(side=action, price=price, quantity=qty)
                except Exception as e:
                    print(f"{RED}Error parsing order: {e}{RESET}")

            elif action == "cancel":
                if len(cmd) < 2:
                    print(f"{RED}Usage: cancel <order_id>{RESET}")
                    continue
                try:
                    oid = int(cmd[1])
                    await self.cancel_order(oid)
                except Exception as e:
                    print(f"{RED}Invalid order ID: {e}{RESET}")

            elif action == "sub":
                await self.subscribe()

            elif action == "ladder":
                self.render_depth_ladder()

            elif action == "snapshot":
                await self.request_snapshot()

            else:
                print(f"{RED}Unknown command '{action}'. Type 'help' for usage.{RESET}")

        if self.ws:
            await self.ws.close()

async def main():
    parser = argparse.ArgumentParser(description="OrderBook Reference WebSocket Client")
    parser.add_argument("--host", default="127.0.0.1", help="Gateway server host")
    parser.add_argument("--port", type=int, default=9001, help="Gateway server port")
    parser.add_argument("--user", type=int, default=1, help="User ID for session")
    parser.add_argument("--sub", action="store_true", help="Auto-subscribe to public feeds on connect")
    args = parser.parse_args()

    uri = f"ws://{args.host}:{args.port}"
    client = OrderBookClient(uri, args.user)

    await client.connect()
    if args.sub:
        await client.subscribe()

    listen_task = asyncio.create_task(client.listen_loop())
    await client.interactive_shell()
    listen_task.cancel()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nExiting.")
