#include <benchmark/benchmark.h>
#include "orderbook/order.hpp"
#include "orderbook/fill.hpp"

using namespace orderbook;

// Benchmark Order construction — baseline cost of creating an order
static void BM_OrderConstruct(benchmark::State& state) {
    uint64_t id = 1;
    for (auto _ : state) {
        Order o;
        o.id        = id++;
        o.side      = Side::Buy;
        o.type      = OrderType::Limit;
        o.status    = OrderStatus::Accepted;
        o.price     = 100;
        o.quantity  = 50;
        o.filled    = 0;
        o.timestamp = 1000;
        benchmark::DoNotOptimize(o);
    }
}
BENCHMARK(BM_OrderConstruct);

// Benchmark open_quantity — trivial arithmetic, good sanity check
static void BM_OpenQuantity(benchmark::State& state) {
    Order o;
    o.quantity = 100;
    o.filled   = 37;
    for (auto _ : state) {
        benchmark::DoNotOptimize(o.open_quantity());
    }
}
BENCHMARK(BM_OpenQuantity);

BENCHMARK_MAIN();