# Benchmark Results

## Baseline — Milestone 3 (std::map + std::deque + std::unordered_map)

**Machine:** 12 x 2611 MHz, L1 48KB, L2 1280KB, L3 12288KB  
**Compiler:** GCC 15.2.0  
**Build:** Release

| Benchmark             | N    | Time (ns) | CPU (ns) | Items/sec |
|-----------------------|------|-----------|----------|-----------|
| BM_LimitInsert        | 10   | 4,623     | 4,653    | 4.30 M/s  |
| BM_LimitInsert        | 100  | 47,705    | 48,205   | 4.15 M/s  |
| BM_LimitInsert        | 1000 | 478,315   | 475,324  | 4.21 M/s  |
| BM_Cancel             | 100  | 6,220     | 5,755    | 17.38 M/s |
| BM_Cancel             | 1000 | 38,900    | 32,017   | 31.23 M/s |
| BM_LimitMatch         | 10   | 601       | 734      | 13.62 M/s |
| BM_LimitMatch         | 100  | 3,479     | 4,028    | 24.82 M/s |
| BM_LimitMatch         | 1000 | 28,732    | 30,691   | 32.58 M/s |
| BM_MixedWorkload      | 100  | 11,060    | 11,300   | 17.70 M/s |
| BM_MixedWorkload      | 500  | 50,228    | 62,779   | 15.93 M/s |

## Observations
- LimitInsert throughput is flat ~4.2M/s regardless of N — dominated by
  std::map tree traversal cost (pointer chasing, poor cache locality).
- Cancel improves with N because the unordered_map lookup is O(1) but
  the deque linear scan to find the order is the bottleneck.
- LimitMatch improves with N because amortized overhead per fill drops.

## Next: Milestone 4 optimizations
Target areas:
1. Replace std::map with a flat array indexed by price tick offset
2. Replace std::deque with an intrusive linked list for O(1) cancel
3. Eliminate per-order heap allocation