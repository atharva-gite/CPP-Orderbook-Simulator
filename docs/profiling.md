# Pipeline profiling

Host: Apple M4 (arm64), Darwin 24.3.0, 10 logical CPUs.  
Build: `Release`, `-O3 -march=native`, C++20. Driver: `bench/profile_pipeline.cpp` (`lob_profile`).  
Workload: 400 000 events after a 20 000-step warmup. Mix is the Phase-4 simulator mix (limit 0.70 / market 0.15 / cancel 0.15), `tick_span=128`, `target_depth_per_side=100`, routed through `MatchingEngine` then optional UDP multicast `Publisher`. Clock: `std::chrono::steady_clock`.

All times below are **measured**, not estimated. Hardware cache-miss *rates* (L1/LL) are **not** available on this machine — see §Tools.

## Tools that were not available

| Tool | Result |
|---|---|
| `valgrind` / `cachegrind` / `callgrind` | Not installed. Homebrew formula `valgrind` 3.27.1 has **no bottle for arm64 macOS** (`supported: []`). Did not invent cachegrind I1/D1/LL miss ratios. |
| `perf stat` / `perf record` | **Not present** (Linux-only; no `/usr/bin/perf`). No `cache-misses`, `branch-misses`, `instructions`, or `cycles` counters and no perf flamegraph. |
| `gprof` | Not installed (`gprof` not on `PATH`; no Homebrew `binutils` gprof). |
| `xctrace` | Present as `/usr/bin/xctrace` but fails: `tool 'xctrace' requires Xcode, but active developer directory is Command Line Tools`. No Instruments CPU-counter session. |

`/opt/homebrew/bin/sample` is an unrelated Python package. Call graphs used **`/usr/bin/sample`**.

## What we ran instead

1. **Scoped `steady_clock` timers** on `submit_limit`, `submit_market`, `cancel_order`, `try_publish_*` (matching-thread enqueue only).
2. **`/usr/bin/sample`** 1 ms stacks for ~6 s on `lob_profile --events=2000000 --hold --no-md`.
3. Isolated **std::map vs sorted-vector lookup** microbench (400 000 finds at depths 8…512).
4. Isolated **intrusive `Order` list walk** (pool slab vs sequential heap), with `sizeof(Order)` and `sysctl` cache geometry.
5. `sysctl`: `hw.cachelinesize=128`, `hw.l1dcachesize=65536` (64 KiB), `hw.l2cachesize=4194304` (4 MiB).

Reproduce:

```bash
cmake --build build --target lob_profile
./build/lob_profile --events=400000 --no-md
./build/lob_profile --events=400000          # includes multicast publisher
/usr/bin/sample <pid> 6 -file /tmp/sample.txt
```

## Cache behaviour (no cachegrind — structure + timed walks)

`sizeof(Order) = 128`, `alignof(Order) = 64`. `static_assert` requires a multiple of 64; iceberg + stop fields push the struct to **two 64-byte slices**, which is **one 128-byte line** on this M4 (`hw.cachelinesize=128`). One resting order therefore occupies a full reported line.

| Walk | n orders | walks | ns / walk | ns / order |
|---|---:|---:|---:|---:|
| pool intrusive list | 64 | 20 000 | 53.7 | 0.84 |
| heap intrusive list (sequential `aligned new`) | 64 | 20 000 | 53.7 | 0.84 |
| pool | 256 | 8 000 | 497.2 | 1.94 |
| heap | 256 | 8 000 | 498.3 | 1.95 |

**Attribution (pattern, not L1 miss %):**

- **Order pool slab:** walking 64 orders at 0.84 ns/order is consistent with sequential 128 B strides hitting L1 (64 KiB D-cache holds 512 such orders). At 256 orders the walk is 1.94 ns/order — still in-cache; the jump is loop/branch cost more than a hard LLC miss cliff.
- **Heap `Order`:** in this microbench allocations were consecutive, so the heap chain matched the pool times (53.7 vs 53.7 ns at n=64). That is **not** a random-heap miss pattern; a long-lived fragmented heap would look worse. Sample of the real book (below) did **not** show `OrderPool::allocate` as hot — the slab freelist is cheap.
- **`std::map` price nodes:** isolated `find` at book-like depth (see §Lookup). Pointer-chasing RB nodes; cost grows with depth (6.58 ns at 8 keys → 44.79 ns at 512). This is the classic “map node miss” pattern: poor spatial locality versus a contiguous key array.
- **`std::unordered_map` id nodes:** not a cache-line walk; **`sample` attributed add/match to `operator new` / `tiny_malloc_from_free_list` on hash-node emplace and `free_tiny` on erase**. That is allocator traffic, not L1-capacity of the Order slab.

We do **not** report fabricated D1/LL miss rates.

## Lookup microbench: `std::map` vs sorted `vector` (400 000 finds)

Same keys, descending comparator (bid side). First baseline run:

| depth | `std::map::find` (ns) | `vector` + `lower_bound` (ns) |
|---:|---:|---:|
| 8 | 6.58 | 8.70 |
| 32 | 24.98 | 7.15 |
| 64 | 31.25 | 7.93 |
| 128 | **36.04** | **9.60** |
| 256 | 40.95 | 11.18 |
| 512 | 44.79 | 13.08 |

At the simulator’s target (~100 ticks/side) a contiguous ladder is ~**3.8×** faster on *lookup*. At depth 8 the tree still wins (6.58 vs 8.70 ns) — binary search + extra indirection is not free on tiny books.

## Call graph (`/usr/bin/sample`, matching path, `--no-md`)

Instrumentation clocks pollute the profile: collapsed **`mach_continuous_time` = 287** samples on the baseline run (657 stacks on the main thread). Ignore that as engine cost.

**Baseline (std::map prices + `unordered_map` ids), collapsed tops ≥5 (engine-relevant):**

| Symbol | collapsed samples |
|---|---:|
| `tiny_free_no_lock` | 36 |
| `tiny_free_list_add_ptr` | 33 |
| `tiny_malloc_from_free_list` | 25 |
| `OrderBook::add_limit_order` | 19 |
| `OrderBook::cancel_order` | 18 |
| `unordered_map` `__emplace_unique_key_args` | 18 |
| `unordered_map` `remove` | 16 |
| `OrderBook::market_order` | 15 |
| `__hash_table::__erase_unique` | 9 |
| `__tree_remove` (std::map) | 6 |

`OrderPool::allocate` appeared in **4** samples under `add_limit_order`. The pool is not the hot miss source.

**After the changes below:** `__hash_table` lines in the sample file went from **26 to 0**. `__tree_*` from **5 to 0**. `tiny_malloc*` lines from **38 to 9**. New collapsed top: **`_platform_memmove` = 59** (price-ladder `vector::insert` shifting `PriceLevel` nodes). Remaining `tiny_malloc_should_clear` (5) is consistent with `std::vector<Fill>` growth inside `market_order`.

## Pipeline timers — baseline (std::map + unordered_map)

400 000 events, `--no-md` (book + matching only):

| path | n | mean ns | total ms |
|---|---:|---:|---:|
| `submit_limit` | 280 218 | 96.5 | 27.033 |
| `submit_market` | 60 213 | 455.7 | 27.439 |
| `cancel_order` | 59 569 | 180.7 | 10.765 |
| event (matching thread) | 400 000 | 236.0 | 94.419 |

Wall: **0.1050 s**, **3 808 590 events/s**.

With multicast publisher (`md=1`): event mean **285.7 ns**, **3 204 442 events/s**, enqueue mean **66.7 ns**, 31 618 packets / 480 645 messages, 0 drops.

`submit_market` is ~4.7× `submit_limit` on the matching thread: fill-vector allocate + hash erase per fill + walking the inside.

## Change 1: `std::map` → `PriceLadder` (sorted vector)

File: `include/lob/price_ladder.hpp`. Bids stay descending (`BidCmp`), asks ascending (`std::less`). `find` / `find_or_emplace` use `lower_bound`; empty levels `erase`. Ladders `reserve(256)` in `OrderBook` ctor.

**Why:** lookup microbench at depth 128 (36.04 vs 9.60 ns).

**Pipeline after ladder only, same 400 000 `--no-md` mix:**

| path | mean ns | vs baseline |
|---|---:|---|
| `submit_limit` | 103.6 | +7.1 ns |
| `submit_market` | 495.4 | +39.7 ns |
| `cancel_order` | 206.2 | +25.5 ns |
| event | 253.0 | +17.0 ns |
| events/s | 3 581 483 | −6.0% |

Lookup got cheaper; **insert/erase got more expensive** (`memmove` of `PriceLevel` on interior ticks). Occupied depth in this mix is often small (map was already fast at depth 8). The ladder alone is a **regression** on the mixed pipeline. Left in place because lookup still wins at 32+ levels and the next change dominates the net result.

## Change 2: `unordered_map` id table → dense `vector<Order*>`

`order_id` is monotonic from 1. `id_slots_[id]` is the order pointer (nullptr if dead). `put_id` doubles capacity; destructor walks the slot array.

**Why:** sample showed hash-node `tiny_malloc` / `free_tiny` on every add and fill, not the Order slab.

### Before / after (combined ladder + dense ids vs original baseline)

Same binary flags, 400 000 events, `--no-md`:

| metric | baseline | after | delta |
|---|---:|---:|---|
| `submit_limit` mean | 96.5 ns | **73.5 ns** | **−23.8%** |
| `submit_market` mean | 455.7 ns | **317.4 ns** | **−30.3%** |
| `cancel_order` mean | 180.7 ns | **120.6 ns** | **−33.3%** |
| event mean | 236.0 ns | **190.4 ns** | **−19.3%** |
| events/s | 3 808 590 | **4 617 569** | **+21.2%** |
| wall | 0.1050 s | 0.0866 s | −17.5% |

With publisher (`md=1`):

| metric | baseline | after | delta |
|---|---:|---:|---|
| `submit_limit` | 83.5 ns | 66.6 ns | −20.2% |
| `submit_market` | 425.5 ns | 286.7 ns | −32.6% |
| `cancel_order` | 134.8 ns | 85.2 ns | −36.8% |
| event | 285.7 ns | 250.2 ns | −12.4% |
| enqueue | 66.7 ns | 74.0 ns | +10.9% (noise / batching) |
| events/s | 3 204 442 | 3 623 629 | +13.1% |

The dense id table is the change that matches the sample (hash malloc gone). The ladder is the change that matches the lookup bench; it shows up after as `_platform_memmove`. Net pipeline is still faster than the original map+hash book.

Catch2 `lob_order_tests`, `lob_matching_tests`, and `lob_tests` passed after both changes.

## Publisher

Matching-thread `try_publish_*` is SPSC enqueue: **66.7 ns** mean baseline, **74.0 ns** after (same 329 761 enqueues). That is not `sendto` latency; the dedicated publisher thread batches and sends. Full-process `sample` with `--md` would be dominated by `cpu_relax` / spinning `recv`/`send` — `--no-md` was used for book/match attribution.

## Still on the table (measured, not done)

- `market_order` still returns `std::vector<Fill>` by value; post-change sample still has `tiny_malloc_should_clear` (5 collapsed). A reused fill buffer would be the next malloc cut.
- `Order` is 128 B; packing iceberg/stop into 64 B would put two orders per 128 B line. Not done — the pool walk is already ~0.8–2 ns/order and was not the sample hotspot.
- Price-ladder `insert` memmove (59 collapsed samples). A small-depth hybrid (map below ~16, vector above) was not implemented; numbers at depth 8 say the tree is still better there.

## Files

- Driver: `bench/profile_pipeline.cpp`
- Ladder: `include/lob/price_ladder.hpp`
- Id table + book: `include/lob/order_book.hpp`, `src/order_book.cpp`
- Raw logs from this session: `build/profile_baseline*.txt`, `build/profile_after*.txt`, `build/sample_baseline.txt`, `build/sample_after.txt`
