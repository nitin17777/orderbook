#include <benchmark/benchmark.h>
#include "orderbook/book.hpp"

using namespace orderbook;

// ── Helpers ──────────────────────────────────────────────────────────────────

static Order make_limit(OrderId id, Side side, Price price, Quantity qty) {
    Order o{};
    o.id        = id;
    o.side      = side;
    o.type      = OrderType::Limit;
    o.status    = OrderStatus::Accepted;
    o.price     = price;
    o.quantity  = qty;
    o.filled    = 0;
    o.timestamp = id; // use id as logical timestamp
    return o;
}

static Order make_market(OrderId id, Side side, Quantity qty) {
    Order o{};
    o.id        = id;
    o.side      = side;
    o.type      = OrderType::Market;
    o.status    = OrderStatus::Accepted;
    o.price     = 0;
    o.quantity  = qty;
    o.filled    = 0;
    o.timestamp = id;
    return o;
}

// ── Benchmark 1: Insert only (no matching) ───────────────────────────────────
// Measures pure resting cost — map insertion + deque push_back + index update.
// Uses alternating prices to build a realistic multi-level book.

static void BM_LimitInsert(benchmark::State& state) {
    const int num_levels = state.range(0);
    OrderId id = 1;

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book;
        state.ResumeTiming();

        for (int i = 0; i < num_levels; ++i) {
            // Spread buys and sells so they don't match
            book.add(make_limit(id++, Side::Buy,  100 - i, 10));
            book.add(make_limit(id++, Side::Sell, 101 + i, 10));
        }
        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations() * num_levels * 2);
}
BENCHMARK(BM_LimitInsert)->Arg(10)->Arg(100)->Arg(1000);

// ── Benchmark 2: Cancel ───────────────────────────────────────────────────────
// Pre-fills the book, then measures cancel throughput.

static void BM_Cancel(benchmark::State& state) {
    const int num_orders = state.range(0);

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book;
        for (int i = 1; i <= num_orders; ++i)
            book.add(make_limit(i, Side::Buy, 100 - (i % 50), 10));
        state.ResumeTiming();

        for (int i = 1; i <= num_orders; ++i)
            book.cancel(static_cast<OrderId>(i));

        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations() * num_orders);
}
BENCHMARK(BM_Cancel)->Arg(100)->Arg(1000);

// ── Benchmark 3: Match (limit aggressor sweeping resting orders) ──────────────
// Measures matching throughput — the hot path in any real engine.

static void BM_LimitMatch(benchmark::State& state) {
    const int num_makers = state.range(0);
    OrderId id = 1;

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book;
        // Place num_makers sell orders at the same price (single level sweep)
        for (int i = 0; i < num_makers; ++i)
            book.add(make_limit(id++, Side::Sell, 100, 1));
        state.ResumeTiming();

        // One large buy sweeps them all
        book.add(make_limit(id++, Side::Buy, 100, static_cast<Quantity>(num_makers)));
        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations() * num_makers);
}
BENCHMARK(BM_LimitMatch)->Arg(10)->Arg(100)->Arg(1000);

// ── Benchmark 4: Realistic mixed workload ─────────────────────────────────────
// Insert, cancel, and market orders interleaved — closest to real order flow.

static void BM_MixedWorkload(benchmark::State& state) {
    const int n = state.range(0);
    OrderId id = 1;

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book;
        // Seed the book with resting orders on both sides
        for (int i = 0; i < n; ++i) {
            book.add(make_limit(id++, Side::Buy,  100 - (i % 10), 10));
            book.add(make_limit(id++, Side::Sell, 101 + (i % 10), 10));
        }
        state.ResumeTiming();

        // Mixed: small market buys and cancels
        for (int i = 0; i < n; ++i) {
            book.add(make_market(id++, Side::Buy, 5));
            book.cancel(static_cast<OrderId>(i + 1));
        }
        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations() * n * 2);
}
BENCHMARK(BM_MixedWorkload)->Arg(100)->Arg(500);

BENCHMARK_MAIN();