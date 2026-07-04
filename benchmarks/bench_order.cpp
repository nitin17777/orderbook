#include <benchmark/benchmark.h>
#include "orderbook/order.hpp"

static void BM_Placeholder(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(orderbook::placeholder());
    }
}
BENCHMARK(BM_Placeholder);

BENCHMARK_MAIN();