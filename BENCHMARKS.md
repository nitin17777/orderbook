# Benchmark Results

**Machine:** 12 x 2611 MHz, L1 48KB, L2 1280KB, L3 12288KB
**Compiler:** GCC 15.2.0
**Build:** Release (-O2)

---

## Results: Naive vs Fast

| Benchmark          | N    | Naive (ns) | Fast (ns) | Speedup |
|--------------------|------|------------|-----------|---------|
| LimitInsert        | 10   | 4,401      | 1,945     | 2.3x ✅ |
| LimitInsert        | 100  | 33,297     | 18,230    | 1.8x ✅ |
| LimitInsert        | 1000 | 180,930    | 174,457   | 1.0x    |
| Cancel             | 100  | 10,073     | 3,561     | 2.8x ✅ |
| Cancel             | 1000 | 65,522     | 31,743    | 2.1x ✅ |
| LimitMatch         | 10   | 1,140      | 8,061     | 0.14x ⚠️|
| LimitMatch         | 100  | 5,301      | 13,126    | 0.40x ⚠️|
| LimitMatch         | 1000 | 41,854     | 52,620    | 0.80x ⚠️|
| MixedWorkload      | 100  | 18,393     | 10,682    | 1.7x ✅ |
| MixedWorkload      | 500  | 72,814     | 50,760    | 1.4x ✅ |

---

## Analysis

### What improved
**Cancel is 2.1–2.8x faster** — the primary goal of the FastOrderBook design.
Naive cancel 
scans a deque linearly to find the order by id. FastOrderBook marks
the order cancelled in the pool (O(1) hash lookup) and skips it lazily during
matching. No scan needed.

**Insert is 1.8–2.3x faster at small-to-medium book depth** — flat array
indexed by price tick gives O(1) level access vs O(log n) std::map traversal.
Advantage narrows at N=1000 because unordered_map pool insertions start
dominating the per-order cost.

**Mixed workload is 1.4–1.7x faster** — real order flow includes cancels,
so the cancel win carries into realistic scenarios.

### What regressed
**LimitMatch is slower in FastOrderBook** — each match step requires an
`unordered_map` lookup into the order pool (`pool_.get(maker_id)`) to
retrieve the maker order. In the naive implementation, the Order object
sits directly in the deque — no lookup required. This is a deliberate
design tradeoff: the pool enables O(1) cancel at the cost of one extra
hash lookup per fill.

At N=1000 (large sweeps) the gap narrows to 1.25x because the per-fill
lookup cost amortizes across many fills per operation.

### Design tradeoff summary
FastOrderBook optimizes for cancel-heavy workloads (typical in real markets
where cancel-to-trade ratios of 10:1 or higher are common). If the workload
were pure matching with no cancels, the naive implementation would win.

### What a further optimization would look like
Replace the `unordered_map` order pool with a flat array indexed by OrderId
(requires bounded OrderId range). Pool lookup becomes a direct array offset —
O(1) with no hashing, cache-friendly. This would recover the match regression
while keeping the cancel win.

---

## Throughput Summary (items/sec)

| Operation     | Naive      | Fast       |
|---------------|------------|------------|
| Insert/1000   | 11.2 M/s   | 10.4 M/s   |
| Cancel/1000   | 13.3 M/s   | 35.7 M/s   |
| Match/1000    | 19.4 M/s   | 17.9 M/s   |
| Mixed/500     | 17.9 M/s   | 20.0 M/s   |