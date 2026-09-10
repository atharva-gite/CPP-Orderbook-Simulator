# Technical writeup

Standalone C++20 limit-order book and matching engine. This note uses **measured** numbers from Phase 1 (allocation counter), Phase 5 (`lob_bench` histograms), Phase 6 (`lob_contention_bench` + TSan), Phase 7 (UDP multicast loopback), and Phase 8 (`lob_profile` + `/usr/bin/sample`). It does not invent hardware counter rates.

Host throughout: Apple M4, Darwin 24.3.0, `hardware_concurrency() = 10`, `hw.cachelinesize = 128`, L1D 64 KiB, L2 4 MiB. Clock: `std::chrono::steady_clock` (empty `now()-now()` **p50 = 42 ns**).

---

## 1. Cache locality: intrusive list + slab vs `std::list` + `new`

`Order` is `alignas(64)` and **128 bytes** (iceberg/stop fields overflow one 64-byte slice; on this CPU that is one reported cache line). The book stores FIFO at a price as an **intrusive** `prev`/`next` on the `Order` itself. The pool is one aligned slab plus a pre-reserved freelist.

A naive comparator was added in `bench/profile_pipeline.cpp` for this writeup: `std::list` of a 128-byte payload, allocated with `new` (libc list nodes). Two layouts: **sequential** `push_back`, and **scattered** (three 64-byte junk allocations between nodes). Walk cost is nanoseconds per full list traversal.

| n | payload | pool intrusive ns/walk | heap-Order intrusive | `std::list` sequential | `std::list` scattered |
|---:|---|---:|---:|---:|---:|
| 64 | 8 KiB | 53.6 | 53.9 | 47.5 | 48.3 |
| 256 | 32 KiB | 497.1 | 498.8 | 495.7 | 509.6 |
| 2048 | 256 KiB | 6337.1 | 1072.3† | 20758.7 | 17473.0 |
| 8192 | 1 MiB | **25787.6** | **26415.3** | **100737.3** | **92176.0** |

Per-order at **n = 8192**: pool **3.15 ns**, heap Order **3.22 ns**, sequential `std::list` **12.3 ns** (~**3.9×** the slab walk).

At 64 and 256 the working set fits L1D (64 KiB holds 512×128 B orders). Walks are indistinguishable; **locality does not show up until the chain leaves L1**. The 2048-row heap-Order number (1072 ns) is not used as a conclusion: that walk ran immediately after the heap allocate loop, so it is consistent with a warm cache / first-touch artifact. At 8192, pool and heap intrusive match.

Scattered vs sequential `std::list` did **not** produce a large extra penalty on this machine at these sizes (92 µs vs 101 µs at 8192). The list node still carries two pointers **plus** allocator headers beside the 128 B payload; the intrusive design keeps the hot `remaining_qty`/`next` on the same object the matcher already has.

**Cachegrind was not run.** Homebrew `valgrind` has no arm64 bottle; `perf` is absent. There is no L1/LL miss-rate table. The 3.9× walk delta at 8192 is the locality evidence we have.

---

## 2. Memory allocation strategy

### Phase 1 counter

`tests/test_order_pool.cpp` overrides `operator new` / aligned `new`. After constructing `OrderPool(1e6)` and `reserve`ing a holder vector, the counter is reset. **1 000 000** `allocate` + `deallocate` then **`g_alloc_count == 0`**. The slab and freelist storage are paid at construction, not per order.

Multicast publish path used the same trick: 400 messages, **`hot_allocs == 0`**.

### What the pool actually saves

Phase 5 (`AllocMode::Pool` vs `Heap` on the real `OrderBook`, depth = distinct ticks per side):

| op | depth | pool ns/op | heap ns/op |
|---|---:|---:|---:|
| `add_limit_order` | 10 | 50.3 | 120.3 |
| `add_limit_order` | 10000 | 71.1 | 173.1 |
| `cancel_order` | 10 | 65.6 | 187.8 |
| sim events/s | 10 | **5.57e6** | 3.61e6 |
| sim events/s | 10000 | **2.90e6** | 1.53e6 |

Pool add is ~**2.4×** the heap toggle at both ends of the depth sweep. Heap is still `aligned new` of `Order`, not `std::list`.

Churn microbench (200 000 alloc+free, Phase 8 driver, current tree):

| path | ns / (alloc+free) |
|---|---:|
| pool freelist | **21.17** |
| `Order` aligned `new`/`delete` | 1298.32 |
| `std::list<Order>` emplace+clear | 285.91 |

The pool wins on **allocator traffic**, not on a 64-order walk. Phase 8 `/usr/bin/sample` agrees: `OrderPool::allocate` was **4** samples under `add_limit_order`; the hot malloc was **`unordered_map` hash nodes**, later removed.

---

## 3. Latency (Phase 5) and what the tail is (Phase 8, not cachegrind)

Clock overhead **p50 = 42 ns**. Pool add/cancel/market **p50 is one or two quanta** — treat those p50s as a floor, not engine time.

### Phase 5 ring-buffer percentiles (pool)

| op | depth | p50 | p99 | p99.9 | ns/op |
|---|---:|---:|---:|---:|---:|
| add_limit_order | 10 | 42 | 84 | 84 | 50.3 |
| add_limit_order | 100 | 42 | 84 | 84 | 50.4 |
| add_limit_order | 1000 | 42 | 84 | 125 | 50.8 |
| add_limit_order | 10000 | 83 | 84 | 125 | 71.1 |
| cancel_order | 10 | 83 | 84 | 125 | 65.6 |
| cancel_order | 100 | 83 | 125 | 166 | 78.3 |
| cancel_order | 1000 | 83 | 125 | 167 | 84.9 |
| cancel_order | 10000 | 125 | 167 | 186 | 107.9 |
| market_order (1 lot inside) | 10…10000 | 83 | 125 | 125–167 | 82–88 |

Heap add at depth 10000: **p50 = 167, p99 = 250, p99.9 = 686, ns/op = 173.1**. That is the only Phase 5 row with a **clear p99.9 tail** (686 vs 167 p50).

`market_order` p50 is **flat vs depth** (83 ns from 10 to 10 000). `begin()` on the then-`std::map` (and now `PriceLadder` front) is O(1); tree size is not on the 1-lot inside path.

Add/cancel ns/op only moves at thousands of ticks (pool add 50.3 → 71.1 at 10k). Log-n of the price container is visible in the **mean**, not as a p99.9 explosion on the pool path.

### Tail attribution — what Phase 8 actually sampled

**No cachegrind, no callgrind.** `/usr/bin/sample` on the matching thread (baseline book: `std::map` + `unordered_map`), collapsed tops:

| symbol | samples |
|---|---:|
| `tiny_free_no_lock` | 36 |
| `tiny_malloc_from_free_list` | 25 |
| `unordered_map` emplace | 18 |
| `unordered_map` remove | 16 |
| `add_limit_order` / `cancel` / `market_order` | 19 / 18 / 15 |
| `__tree_remove` (`std::map`) | **6** |

So the dominant cost on the **mixed pipeline** was **hash-node malloc/free**, not map rebalancing. `__tree_remove` is present but an order of magnitude below allocator. We **cannot** assign Phase 5’s heap-add **p99.9 = 686 ns** to an L1 miss rate; the honest statement is: heap `aligned new` of `Order` plus libc (Phase 5 heap vs pool), and on the live book, **id-map nodes** (Phase 8 sample). After replacing the id table with `vector<Order*>`, `__hash_table` lines in the sample file went **26 → 0**; remaining `tiny_malloc` is consistent with `std::vector<Fill>` in `market_order`.

Phase 8 matching-thread **means** (400k events, after those changes, `--no-md`): `submit_limit` **73.5 ns**, `submit_market` **317.4 ns**, `cancel` **120.6 ns**, event **190.4 ns**, **4.62M events/s**. Market is still ~4× limit because of fills + vector, not because of `std::map` rebalance.

---

## 4. Concurrency

### What is lock-free vs not

| piece | claim we will stand behind |
|---|---|
| `MpscRing::try_push` | **Lock-free, not wait-free.** CAS on `enqueue_pos_`. A producer can starve. **Full** ring: `try_push` returns false; spinning on that is **not** lock-free progress. |
| `MpscRing::try_pop` | **Wait-free** for the single consumer if non-empty. |
| `SpscRing` (matching → MD) | **Wait-free** try_push/try_pop when not full/empty. No CAS. |
| `SeqlockQuote::read` | **Not lock-free.** Spin while seq is odd. Writer never waits on readers. A hot writer can starve readers. |
| `ShardedBooks` | **`std::mutex` per shard.** Blocking. Less contended than one global lock if work is spread across instruments. Same instrument still serializes. |

TSan (`-DLOB_ENABLE_TSAN=ON`): concurrent tests + contention bench, `halt_on_error=1`, **no data-race reports**. Seqlock test: readers never observe mixed fields from two publishes (`bid` tied to `bid_qty` in the test). `applied` matched expected counts (no lost updates).

Sharding is by **instrument id**, not price. A market order must walk one book.

### Seqlock vs sharded books (Phase 6, Release)

`p50`/`p99` = producer submit wait (MPSC success including full-queue spin, or mutex hold). 40 000 ops/producer.

| model | N | ingest/s | p50_ns | p99_ns |
|---|---:|---:|---:|---:|
| single-writer+MPSC | 1 | 16.0e6 | 42 | 42 |
| sharded-mutex | 1 | 10.9e6 | 42 | 167 |
| single-writer+MPSC | 2 | 9.4e6 | 167 | 334 |
| sharded-mutex | 2 | 5.2e6 | 83 | 6292 |
| single-writer+MPSC | 4 | 6.1e6 | 459 | 2500 |
| sharded-mutex | 4 | 6.2e6 | 125 | 8375 |
| single-writer+MPSC | 8 | 4.0e6 | 1250 | 12500 |
| sharded-mutex | 8 | 5.0e6 | 167 | 31667 |
| single-writer+MPSC | 10 | 3.5e6 | 1875 | 13750 |
| sharded-mutex | 10 | 4.9e6 | 167 | 39333 |

Single-writer ingest **falls** as N grows: one consumer, one book; extra producers fight the MPSC. Sharded ingest holds ~5e6/s at N=8–10 because this bench hashes across **N instruments**. That is topology, not “mutexes beat lock-free.” Sharded **p99 is ugly** (39 µs at N=10): lock hold + convoying. MPSC p99 at N=10 is 13.8 µs of queueing, not mutex.

### What this machine cannot demonstrate

Ten laptop cores. N=8 and N=10 are **oversubscription**, not a NUMA scaling study. We did not measure cross-socket invalidation, isolated matching vs publisher cores (macOS affinity is a **hint**, not `pthread_setaffinity_np`), or seqlock reader throughput under a 1 MHz quote writer. TSan proves absence of C++ data races in the tests we ran; it does not prove wait-freedom or bounded p99 on a 64-core box.

---

## 5. Networking vs matching

Loopback multicast (`239.1.2.3:19001`, 400 messages, 11 datagrams, **0** publish-hot-path heap allocs):

| metric | p50 | p99 |
|---|---:|---:|
| serialize / message | **6 ns** | 83 ns |
| publish (event timestamp → `sendto` return) | **107 µs** | 129 µs |
| round-trip (event → recv decode) | **200 µs** | 235 µs |

Matching-thread enqueue (`try_publish_*` SPSC only), Phase 8 pipeline with MD: **74.0 ns** mean (329 761 enqueues). Event on the matching thread with MD: **250.2 ns** mean; without MD: **190.4 ns**. `submit_market` **286.7 ns** mean with MD.

Relative sizes, same host:

- Pack one ITCH-style message: **~6 ns** (in the noise of `steady_clock`).
- Match a limit: **~67–74 ns** mean after the id-table change; Phase 5 pool add **~50 ns/op** sitting on the 42 ns clock.
- Match a market: **~287–317 ns** mean.
- Enqueue to the publisher: **~67–74 ns**.
- Event → UDP `sendto`: **~107 µs p50**, because the producer fills the SPSC faster than the publisher drains and **early events wait in a batch** (11 packets / 400 msgs). That is not serialization.
- Loopback RTT **~200 µs p50**: includes kernel UDP, batching, and the busy-poll receiver still issuing `recvfrom` syscalls.

Busy-poll on macOS is not Linux `SO_BUSY_POLL`. Empty polls are still syscalls. Publisher spin (`cpu_relax` when the SPSC is empty) would dominate a whole-process sample; matching attribution used `--no-md`.

---

## What we would redo

**1. Stop returning `std::vector<Fill>` from `market_order` / `submit_market`.**  
After killing the id `unordered_map`, sample still showed `tiny_malloc_should_clear` (5 collapsed) on that path. Phase 8 market mean **317 ns** vs limit **74 ns**. A reused fill buffer (or a small in-struct array plus overflow) is the obvious next cut. We measured the symptom and left the API.

**2. Price ladder: vector insert vs tree.**  
Isolated `lower_bound` at depth 128: **9.60 ns** vs `std::map::find` **36.04 ns**. Mixed pipeline after switching **only** the ladder: events/s **3.81M → 3.58M** (regression). Post-change sample: **`_platform_memmove` = 59** collapsed. Depth 8 still prefers the tree (6.58 vs 8.70 ns). A hybrid (tree below ~16 ticks, vector above) or a fixed tick array for the inside N levels would match both benches. We kept the vector and papered the regression with the id-table win.

**3. Publisher batching vs latency, and `Order` at 128 B.**  
Publish p50 **107 µs** is batching wait, not 6 ns serdes. For a market-data thread that should not wait for 64 messages, flush on a time budget (or when the matching thread goes idle). Separately, `Order` wastes a full 128 B line; packing iceberg/stop into 64 B was not the sample hotspot (pool walk 0.8–3 ns/order) but it doubles L1 residency. The dense `id_slots_` vector also **never shrinks** as ids increase — fine for a bench, not fine for a multi-day process.

Smaller: `kCacheLine` is 64 in software while `sysctl` reports 128; seqlock/MPSC padding may not split lines the way we think on this core.
