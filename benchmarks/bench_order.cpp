#include <benchmark/benchmark.h>
#include "orderbook/book.hpp"
#include "orderbook/fast_book.hpp"

using namespace orderbook;

// ── Helpers ──────────────────────────────────────────────────────────────────

static Order make_limit(OrderId id, Side side, Price price, Quantity qty) {
    Order o{};
    o.id = id; o.side = side; o.type = OrderType::Limit;
    o.status = OrderStatus::Accepted;
    o.price = price; o.quantity = qty; o.filled = 0; o.timestamp = id;
    return o;
}

static Order make_market(OrderId id, Side side, Quantity qty) {
    Order o{};
    o.id = id; o.side = side; o.type = OrderType::Market;
    o.status = OrderStatus::Accepted;
    o.price = 0; o.quantity = qty; o.filled = 0; o.timestamp = id;
    return o;
}

// ── LimitInsert ───────────────────────────────────────────────────────────────

static void BM_LimitInsert_Naive(benchmark::State& state) {
    const int n = state.range(0);
    OrderId id = 1;
    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book;
        state.ResumeTiming();

        for (int i = 0; i < n; ++i) {
            book.add(make_limit(id++, Side::Buy,  100 - (i % 50), 10));
            book.add(make_limit(id++, Side::Sell, 101 + (i % 50), 10));
        }
        benchmark::DoNotOptimize(book);
    }
    state.SetItemsProcessed(state.iterations() * n * 2);
}
BENCHMARK(BM_LimitInsert_Naive)->Arg(10)->Arg(100)->Arg(1000);

static void BM_LimitInsert_Fast(benchmark::State& state) {
    const int n = state.range(0);
    OrderId id = 1;
    FastOrderBook book; // constructed ONCE outside the loop
    for (auto _ : state) {
        state.PauseTiming();
        book.reset();   // cheap — clears contents, keeps 10k slots allocated
        state.ResumeTiming();

        for (int i = 0; i < n; ++i) {
            book.add(make_limit(id++, Side::Buy,  100 - (i % 50), 10));
            book.add(make_limit(id++, Side::Sell, 101 + (i % 50), 10));
        }
        benchmark::DoNotOptimize(book);
    }
    state.SetItemsProcessed(state.iterations() * n * 2);
}
BENCHMARK(BM_LimitInsert_Fast)->Arg(10)->Arg(100)->Arg(1000);

// ── Cancel ────────────────────────────────────────────────────────────────────

static void BM_Cancel_Naive(benchmark::State& state) {
    const int n = state.range(0);
    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book;
        for (int i = 1; i <= n; ++i)
            book.add(make_limit(i, Side::Buy, 100 - (i % 50), 10));
        state.ResumeTiming();

        for (int i = 1; i <= n; ++i)
            book.cancel(static_cast<OrderId>(i));
        benchmark::DoNotOptimize(book);
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Cancel_Naive)->Arg(100)->Arg(1000);

static void BM_Cancel_Fast(benchmark::State& state) {
    const int n = state.range(0);
    FastOrderBook book;
    for (auto _ : state) {
        state.PauseTiming();
        book.reset();
        for (int i = 1; i <= n; ++i)
            book.add(make_limit(i, Side::Buy, 100 - (i % 50), 10));
        state.ResumeTiming();

        for (int i = 1; i <= n; ++i)
            book.cancel(static_cast<OrderId>(i));
        benchmark::DoNotOptimize(book);
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Cancel_Fast)->Arg(100)->Arg(1000);

// ── LimitMatch ────────────────────────────────────────────────────────────────

static void BM_LimitMatch_Naive(benchmark::State& state) {
    const int n = state.range(0);
    OrderId id = 1;
    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book;
        for (int i = 0; i < n; ++i)
            book.add(make_limit(id++, Side::Sell, 100, 1));
        state.ResumeTiming();

        book.add(make_limit(id++, Side::Buy, 100, static_cast<Quantity>(n)));
        benchmark::DoNotOptimize(book);
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_LimitMatch_Naive)->Arg(10)->Arg(100)->Arg(1000);

static void BM_LimitMatch_Fast(benchmark::State& state) {
    const int n = state.range(0);
    OrderId id = 1;
    FastOrderBook book;
    for (auto _ : state) {
        state.PauseTiming();
        book.reset();
        for (int i = 0; i < n; ++i)
            book.add(make_limit(id++, Side::Sell, 100, 1));
        state.ResumeTiming();

        book.add(make_limit(id++, Side::Buy, 100, static_cast<Quantity>(n)));
        benchmark::DoNotOptimize(book);
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_LimitMatch_Fast)->Arg(10)->Arg(100)->Arg(1000);

// ── MixedWorkload ─────────────────────────────────────────────────────────────

static void BM_MixedWorkload_Naive(benchmark::State& state) {
    const int n = state.range(0);
    OrderId id = 1;
    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book;
        for (int i = 0; i < n; ++i) {
            book.add(make_limit(id++, Side::Buy,  100 - (i % 10), 10));
            book.add(make_limit(id++, Side::Sell, 101 + (i % 10), 10));
        }
        state.ResumeTiming();

        for (int i = 0; i < n; ++i) {
            book.add(make_market(id++, Side::Buy, 5));
            book.cancel(static_cast<OrderId>(i + 1));
        }
        benchmark::DoNotOptimize(book);
    }
    state.SetItemsProcessed(state.iterations() * n * 2);
}
BENCHMARK(BM_MixedWorkload_Naive)->Arg(100)->Arg(500);

static void BM_MixedWorkload_Fast(benchmark::State& state) {
    const int n = state.range(0);
    OrderId id = 1;
    FastOrderBook book;
    for (auto _ : state) {
        state.PauseTiming();
        book.reset();
        for (int i = 0; i < n; ++i) {
            book.add(make_limit(id++, Side::Buy,  100 - (i % 10), 10));
            book.add(make_limit(id++, Side::Sell, 101 + (i % 10), 10));
        }
        state.ResumeTiming();

        for (int i = 0; i < n; ++i) {
            book.add(make_market(id++, Side::Buy, 5));
            book.cancel(static_cast<OrderId>(i + 1));
        }
        benchmark::DoNotOptimize(book);
    }
    state.SetItemsProcessed(state.iterations() * n * 2);
}
BENCHMARK(BM_MixedWorkload_Fast)->Arg(100)->Arg(500);

BENCHMARK_MAIN();