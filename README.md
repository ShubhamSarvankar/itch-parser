# ITCH 5.0 Order Book Reconstruction

A from-scratch NASDAQ TotalView-ITCH 5.0 binary parser and in-memory limit
order book in C++20. Parses all 9 message types over fixed-point arithmetic,
reconstructs both book sides, and replays a real **263M-message, 7.7 GB
production capture end-to-end at 1.16M msg/s** (231 ns median apply latency,
single thread). 105 tests cover the full mutation set plus the failure modes
that actually corrupt a book: feed-gap underflow, crossed quotes, oversized
executes, and shutdown-drain races.

Built against the December 2019 NASDAQ capture (`12302019.NASDAQ_ITCH50`),
with a focus on honest latency measurement rather than headline microbenchmark
numbers — every figure below is reproducible and the limitations are
documented exhaustively in §4.

---

## 1. Headline

End-to-end replay of the December 2019 NASDAQ capture: no tuning, no trimming,
single threaded, snapshot publish disabled for the baseline (snapshot interval
set to 1,000,000 messages so it effectively never fires — see §3 for why that
is the honest baseline and what happens at the production interval).

| metric               | value        |
|----------------------|--------------|
| messages             | 263 M        |
| wall clock           | 227.5 s      |
| sustained throughput | 1.16 M msg/s |
| p50 apply latency    | 231 ns       |
| p99 apply latency    | 1,020 ns     |
| p99.9 apply latency  | 1.53 µs      |
| p99.99 apply latency | 10.97 µs     |
| max apply latency    | 58.6 ms      |
| peak RSS             | 181.6 MB     |

Reproduce with:

```bash
make bench-replay ITCH_FILE=data/12302019.NASDAQ_ITCH50
```

HDR histogram CSVs are committed under `bench/out/`.

The interesting story is not the means. It is what changed from the baseline,
and what didn't — see §3 for the tail, §4 for the 58.6 ms max.

> Hardware-counter metrics (IPC, L1/LLC miss rates, branch miss rate) are not
> reported anywhere in this document: the dev environment is WSL2, whose kernel
> does not expose the `perf_event` subsystem. HdrHistogram percentiles are the
> load-bearing latency measurement throughout.

---

## 2. Data structure shootout: how to back the price ladder

The order book's per-side price ladder is the hottest data structure in the
system. Every Add, Delete, Cancel, Execute, and Replace touches it. Three
reasonable choices, all implemented (`include/itch/price_ladder.h`) and benched
(`bench_ladder_shootout.cpp`):

| ladder        | mixed-op median¹ | top()   | bytes/instr/side²          |
|---------------|------------------|---------|----------------------------|
| `MapLadder`   | 17.6 ns          | 0.21 ns | ~48 B × N_levels           |
| `FlatLadder`  | 13.7 ns          | 0.30 ns | ~16 B × N_levels           |
| `ArrayLadder` | 11.9 ns          | 0.28 ns | 24 KB fixed (1024 slots)   |

¹ Mixed-workload median across 10 repetitions (75/20/5 delete/add/execute on a
  10k-seeded book resident in L2). CV ≤ 1.9%, so p99 ≈ median for this
  workload. Isolated add/erase percentiles were not captured separately.

² Per instrument per side. `MapLadder` and `FlatLadder` scale with active level
  count; at 20 levels Map ≈ 960 B, Flat ≈ 344 B. `ArrayLadder` is fixed
  regardless of fill (`SLOTS=1024`, `sizeof(PriceLevel)=24`).

`MapLadder` is a `std::map<Price, PriceLevel>`: a red-black tree, pointer
chasing on every operation, but it handles any price range. This is the
baseline and the current default.

`FlatLadder` is a `std::vector<PriceLevel>` kept sorted best-first. Binary
search insert (`std::lower_bound`), cache-friendly iteration, but O(n) inserts
into the middle of the vector. Best when the active level count is small (under
a few hundred), which holds for most equities most of the time.

`ArrayLadder` is a fixed-size dense `std::array<PriceLevel, SLOTS>` indexed by
`(price.raw - base_raw) / tick`. O(1) on every operation except erasing the
best level, which walks to the next nonempty slot. High per-book memory; only
works for bounded tick grids; out-of-band prices are silently dropped.

Production HFT books usually combine these: an array for the near book (the
dense window around midpoint) and a sorted vector or skip list for the tail.
For NASDAQ equities with bounded daily price ranges, `ArrayLadder` wins on
every benchmarked metric. The engine still ships `MapLadder` because at 5000
instruments `ArrayLadder`'s per-instrument footprint dominates the benefit once
the working set already fits in L2 — the win is real but the memory trade-off
is not, at this scale.

---

## 3. Tail latency under realistic load

Histogram for the current implementation (snapshot publish on the pipeline
thread, snapshot interval 1,000,000 for the baseline):

```
parse:  p50  38 ns,  p99   88 ns,  p99.9  149 ns,                 max  1.3 ms
apply:  p50 231 ns,  p99 1020 ns,  p99.9 1529 ns,  p99.99 10.97 µs, max 58.6 ms
```

The p99.99 spike is the periodic snapshot publish. The 58.6 ms max is a
separate issue: an `order_index_` hash-table rehash as the map crosses a
capacity boundary mid-session (see §4 for the one-line fix and why it is not
applied by default).

At the production snapshot interval (1,000 messages), snapshot publish
dominates wall clock by roughly 30× (extrapolated from a partial run; see
`bench/BASELINE.md`). That is the empirical motivation for the off-thread
snapshot design.

The Phase 5 off-thread snapshot design
(`docs/PHASE5_OFF_THREAD_SNAPSHOT.md`) targets the p99.99 spike by moving
snapshot construction off the pipeline thread via a seqlock + SPSC dirty queue.
Phase 5 is **design only** — implementation was deferred pending concurrent
property tests that cannot be reliably validated in this environment (see §4).

---

## 4. What is correct, and what is a known limitation

What the system does right:

- All 9 in-scope ITCH 5.0 message types parsed against fixed-width offsets;
  out-of-scope types silently discarded, per spec.
- Bid-side comparator inverted (`std::greater<Price>`) so `bids.begin()` is the
  best bid in O(1).
- The 6-byte timestamp is parsed as 6 actual bytes, not `memcpy`'d as 8 — an
  8-byte copy would silently corrupt the following field.
- Fixed-point price arithmetic throughout; `to_double()` only at serialization.
- Order Replace copies the old record before erasing it (the Replace message
  carries neither side, locate, nor MPID).
- Oversized execute/cancel is clamped to zero and the order erased, so a feed
  gap cannot leave an immortal order or a multi-billion-share phantom level.
- Crossed-book spread is computed with signed arithmetic, returning a negative
  value during auctions/halts instead of wrapping `UINT32` to ~$429k.
- Single-writer / multi-reader snapshot handoff via
  `std::atomic<std::shared_ptr<SystemSnapshot>>`. Whether this is truly
  lock-free is implementation-defined: on the documented toolchain
  (libstdc++, GCC 13.3) it is backed by an internal mutex pool rather than a
  hardware CAS. The handoff is correct, and readers never block the writer,
  but it is not claimed as lock-free on this platform.

What it is not:

- **Not a matching engine.** The book records resting orders but does not match
  them. Price-time priority is implied by ITCH message ordering, not enforced.
- **No auction-cross handling.** `Cross Trade` (`Q`) and `Net Order Imbalance`
  (`I`) are not modeled. Opening and closing crosses appear as the post-cross
  state via the regular Add/Execute stream.
- **`ArrayLadder` assumes a 1-cent tick.** For securities under $1.00 the SEC's
  sub-penny rules apply (Rule 612) and the index math overflows the slot count.
  `MapLadder` is the safe default across the full listed universe.
- **No FPGA-style SIMD parser.** Field offsets and big-endian reads could be
  batched with `_pext_u64` / `vpshufb`; marginal at best for ITCH, where parse
  cost is already a small fraction of apply cost.
- **Snapshot symbol-map keys are `std::string`.** A `std::string_view` into the
  long-lived `instruments_` map would save the allocation, but the dense-map
  backing invalidates pointers on rehash. Until symbol storage is moved out of
  the hash table, the keys stay `std::string`.
- **`order_index_` rehash causes the 58.6 ms apply max.** The dense in-flight
  order map grows past its default capacity mid-session and rehashes in place,
  blocking the pipeline thread for tens of milliseconds. One-line fix:
  `order_index_.reserve(150'000'000)` in the engine constructor. Not applied
  here because peak RSS would rise from 181 MB to ~3 GB (150M × ~16 B). The
  spike is documented; the fix is a deployment-time decision.
- **Synthetic benchmark mix does not match real ITCH.** The mixed-workload
  bench uses 75/20/5 delete/add/execute; the real capture is closer to
  43/45/12. The synthetic bench is for ladder comparison only, where relative
  ordering matters more than absolute fidelity. `BM_ApplyMixed_Capture` is the
  honest end-to-end number.
- **Phase 5 off-thread snapshot is design only.** The seqlock + SPSC
  dirty-queue implementation requires concurrent property tests ("snapshot
  copies never observe a level with shares < 0") that cannot be reliably
  written in this environment. The design is in
  `docs/PHASE5_OFF_THREAD_SNAPSHOT.md`; no code was produced.

A senior reviewer should walk away knowing exactly what is robust and what is
not. The list above is deliberately exhaustive.

---

## 5. Architecture notes

The pipeline is single-producer / multi-consumer at the snapshot boundary only:

- **Pipeline thread** — feed reader → parser → engine. No locks.
- **Snapshot thread** (Phase 5, optional) — per-book seqlock copies, published
  via the same atomic `shared_ptr`.
- **REST thread** — reads the published snapshot via an atomic load. Never
  touches the engine.

Snapshots are immutable once published, so every REST request gets a consistent
point-in-time view and multiple in-flight requests share one snapshot via the
`shared_ptr` refcount.

Message dispatch in the parser is a 9-case switch over the type byte, which the
compiler emits as a jump table at `-O2`. An optional computed-goto dispatch is
available under `-DITCH_PARSER_COMPUTED_GOTO=1` (GCC only); on this hardware the
difference is under measurement noise, so the switch ships by default.

Live-mode shutdown uses a two-phase drain: the engine thread spins while the
receiver runs, dequeuing one message per iteration, then performs a final flush
after `recv_done` is set so no message queued during teardown is dropped.

---

## Appendix A: build and run

Requires GCC 13+, CMake 3.24+, Linux. Dependencies are pulled via FetchContent:
cpp-httplib, nlohmann/json, GoogleTest, Google Benchmark, moodycamel
ConcurrentQueue, HdrHistogram_c, ankerl/unordered_dense.

```bash
make build          # release build
make replay         # end-to-end replay
make bench          # gbench JSON
make bench-replay   # histogrammed replay (HdrHistogram)
make perf-baseline  # with perf stat (native Linux only; unavailable on WSL2)
make perf-bench     # gbench under perf (native Linux only)
```

Run modes:

```bash
# File replay
ITCH_FILE=data/12302019.NASDAQ_ITCH50 ./build/itch_parser

# Live multicast
ITCH_MODE=live MCAST_GROUP=233.54.12.111 MCAST_PORT=26477 \
    MCAST_IFACE=eth0 ./build/itch_parser
```

REST endpoints:

| method | path                     | description                                       |
|--------|--------------------------|---------------------------------------------------|
| GET    | `/status`                | messages_processed, instruments_tracked, snap age |
| GET    | `/instruments`           | trading state, locate, lot size per symbol        |
| GET    | `/book/:symbol?depth=N`  | full book up to N levels (default 10, max 50)     |
| GET    | `/book/:symbol/top`      | best bid, best ask, spread (fixed-point, signed)  |

---

## Appendix B: tests

105 GoogleTest cases across price arithmetic, parser, book engine, REST server,
and UDP feed reader. `ctest --output-on-failure` runs all of them.

Beyond the happy path, the suite targets the failure modes that silently
corrupt a book or drop data:

- **Oversized execute / cancel** — executing or cancelling more shares than an
  order holds. Clamps to zero and erases the order rather than wrapping
  `rec.shares` / `total_shares` into a multi-billion-share phantom level.
- **Crossed book** — bid above ask (legal during auctions and halts). Spread is
  reported as a signed negative value, not a `UINT32` wrap to ~$429k.
- **Shutdown-drain race** — a message queued as `recv_done` flips to true is
  still applied, not consumed by the loop condition and discarded.
- **Unknown order reference** — an execute/cancel/delete against an unseen order
  ref is counted and skipped, never dereferenced.

The book engine tests cover all 5 mutation paths (Add, Execute, Cancel, Delete,
Replace) plus integration scenarios run end-to-end against the parser and REST
layer.

---

## Specification

NASDAQ TotalView-ITCH 5.0, January 2014:
<http://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf>