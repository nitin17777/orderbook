# Order Book Engine

A price-time priority matching engine written in C++20.
Built for learning — every design decision is deliberate and documented.

---

## What it does

- Accepts **limit orders**, **market orders**, and **cancellations**
- Matches using **price-time priority** (best price first, FIFO within a level)
- Emits **fill events** for every matched quantity
- Logs every command to an **event log** for crash recovery and replay
- Ships with two book implementations — naive and optimized — with benchmarks

---

## Architecture
Caller
│
▼
Engine          — logs every command, then applies it
├── EventLog  — append-only binary log (replay source of truth)
└── OrderBook — price-time priority matching engine
├── BidLevels  (std::map, descending)
└── AskLevels  (std::map, ascending)
└── per level: FIFO queue of Orders




### Two implementations

| | OrderBook (naive) | FastOrderBook (optimized) |
|---|---|---|
| Price levels | `std::map` (O log n) | Flat array indexed by tick (O(1)) |
| Order storage | Inside level queues | Central pool, queues hold ids |
| Cancel | Linear scan O(n) | Lazy O(1) — mark in pool, skip on match |
| Best for | Correctness reference | Cancel-heavy workloads |

---

## Performance

Benchmarked on 12-core 2611 MHz, GCC 15.2.0, Release build.

| Operation | Naive | Fast | Speedup |
|---|---|---|---|
| Insert/1000 | 11.2 M/s | 10.4 M/s | 1.0x |
| Cancel/1000 | 13.3 M/s | 35.7 M/s | 2.7x |
| Match/1000 | 19.4 M/s | 17.9 M/s | 0.9x |
| Mixed/500 | 17.9 M/s | 20.0 M/s | 1.1x |

Full results and analysis in [BENCHMARKS.md](BENCHMARKS.md).

---

## Project structure
orderbook/
├── include/orderbook/
│   ├── types.hpp       # Price, Quantity, OrderId, Side, enums
│   ├── order.hpp       # Order and Fill structs
│   ├── fill.hpp        # Fill event
│   ├── book.hpp        # Naive OrderBook
│   ├── fast_book.hpp   # Optimized FastOrderBook
│   ├── command.hpp     # Command type for event log
│   ├── event_log.hpp   # Append-only binary event log
│   └── engine.hpp      # Engine: log + book combined
├── src/
│   ├── book.cpp
│   ├── fast_book.cpp
│   ├── engine.cpp
│   └── main.cpp        # Interactive CLI
├── tests/
│   ├── test_order.cpp  # Core type and book tests (20 cases)
│   └── test_engine.cpp # Engine and replay tests (5 cases)
├── benchmarks/
│   └── bench_order.cpp # Google Benchmark suite
├── SPEC.md             # Design spec written before any code
├── BENCHMARKS.md       # Benchmark results with analysis
└── CMakeLists.txt



---

## Build

**Requirements:** CMake 3.20+, C++20 compiler, Ninja (optional)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Dependencies (Catch2, Google Benchmark) are fetched automatically via
CMake FetchContent — no manual installation needed.

---

## Run

### Interactive CLI
```bash
./build/orderbook_cli
```

Example session:
add buy 100 10       # limit buy 10 @ 100
add sell 105 5       # limit sell 5 @ 105
book                 # display book
add sell 100 8       # crosses spread — fills against bid
mkt buy 3            # market order
cancel 1             # cancel by id
quit                 # saves log and exits


### Tests
```bash
./build/orderbook_tests
```

### Benchmarks
```bash
./build/orderbook_bench
```

---

## Design decisions

**Integer prices.** All prices are `int64_t` ticks, never `double`.
Floating point arithmetic is non-deterministic across platforms and has
rounding error. Real exchanges use integer ticks with a known tick size.

**Maker price for fills.** When two orders match, the fill price is always
the resting (maker) order's price — standard exchange convention. The
incoming (taker) order gets price improvement if it crossed.

**Market orders never rest.** If a market order cannot be fully filled due
to insufficient liquidity, the remainder is cancelled — never placed in the
book. This matches real exchange behavior.

**Event sourcing.** Every command is written to a binary log before being
applied to the book. The book state is a pure function of the log — replay
the log into a fresh book and you get identical state. This enables crash
recovery and a complete audit trail.

**Lazy cancel in FastOrderBook.** Instead of scanning the queue to remove
a cancelled order, we mark it cancelled in the order pool and skip it during
matching. This makes cancel O(1) at the cost of slightly more work during
matching sweeps.

---

## Known limitations / future work

- Price range is fixed at 1-10,000 ticks (FastOrderBook)
- No order modification (cancel-replace)
- No IOC / FOK order types
- Event log has no header, versioning, or CRC checksums
- Single-threaded only
- No network interface (WebSocket feed would be the natural next step)