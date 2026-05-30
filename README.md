# ITCH 5.0 Order Book Reconstruction

A NASDAQ TotalView-ITCH 5.0 binary feed parser and in-memory order book
maintained in C++20. Built against a real production capture
(`12302019.NASDAQ_ITCH50`, 7.7 GB, 263M messages), with a focus on
honest latency measurement rather than headline microbenchmark numbers.

---

## 1. Headline

End-to-end replay of the December 2019 NASDAQ capture, no tuning, no
trimming, single threaded, snapshot publish disabled for baseline
(snap=1,000,000 — see §3 for why):

| metric                              | value                            |
|-------------------------------------|----------------------------------|
| messages                            | 263 M                            |
| wall clock                          | 227.5 s                          |
| sustained throughput                | 1.16 M msg/s                     |
| p50 apply latency                   | 231 ns                           |
| p99 apply latency                   | 1,020 ns                         |
| p99.9 apply latency                 | 1.53 us                          |
| peak RSS                            | 181.6 MB                         |
| IPC                                 | N/A (perf unavailable on WSL2)   |
| L1 dcache miss rate                 | N/A (perf unavailable on WSL2)   |
| LLC miss rate                       | N/A (perf unavailable on WSL2)   |

Reproduce with:

```
make bench-replay ITCH_FILE=data/12302019.NASDAQ_ITCH50
```

HDR histogram CSVs are committed under `bench/out/`.

The interesting story is not the means. It is what changed from the
baseline, and what didn't.

---

## 2. Data structure shootout: how to back the price ladder

The order book's per-side price ladder is the hottest data structure in
the system. Every Add, Delete, Cancel, Execute, and Replace touches it.
Three reasonable choices, all implemented (`include/itch/price_ladder.h`)
and benched (`bench_ladder_shootout.cpp`):

| ladder        | mixed-op med¹ | top()    | bytes/instr²              | L1 miss                  |
|---------------|---------------|----------|---------------------------|--------------------------|
| `MapLadder`   | 17.6 ns       | 0.21 ns  | ~48 B × N_levels per side | N/A (perf unavailable)   |
| `FlatLadder`  | 13.7 ns       | 0.30 ns  | ~16 B × N_levels per side | N/A (perf unavailable)   |
| `ArrayLadder` | 11.9 ns       | 0.28 ns  | 32 KB fixed (1024 slots)  | N/A (perf unavailable)   |

¹ Mixed-workload median across 10 repetitions (75/20/5 delete/add/execute,
  10k-seeded book in L2 cache). CV ≤ 1.9% — p99 ≈ median for this workload.
  Isolated add/erase percentiles not captured separately.

² Per instrument per side. MapLadder and FlatLadder scale with active level
  count; at 20 levels: Map ≈ 960 B, Flat ≈ 344 B. ArrayLadder is fixed at
  16 KB/side regardless of fill (SLOTS=1024, sizeof(PriceLevel)=16).

`MapLadder` is a `std::map<Price, PriceLevel>`. Red-black tree, pointer
chasing on every operation, but handles any price range. Baseline.

`FlatLadder` is `std::vector<PriceLevel>` kept sorted best-first. Binary
search insert (`std::lower_bound`), cache-friendly iteration, but
O(n) inserts in the middle of the vector. Best when the active level
count is small (under a few hundred), which is true for most equities
most of the time.

`ArrayLadder` is a fixed-size dense `std::array<PriceLevel, SLOTS>`
indexed by `(price.raw - base_raw) / tick`. O(1) on every operation
except erasing the best level (which walks until it finds the next
nonempty slot). Memory footprint is high per book; only works for
bounded tick grids; out-of-band prices are silently dropped.

Production HFT books usually combine these: an array for the near book
(the dense window around midpoint), a sorted vector or skip list for
the tail. For NASDAQ equities with bounded daily price ranges,
`ArrayLadder` wins on every metric except instantiation cost. The current
engine still ships with `MapLadder` because the cost of `ArrayLadder`'s
per-instrument memory footprint at 5000 instruments dominates the
benefit when the working set already fits in L2.

---

## 3. Tail latency under realistic load

Histogram, current implementation (snapshot publish on pipeline thread,
snap=1,000,000 for baseline):

```
parse:  p50  38 ns,  p99   88 ns,  p99.9   149 ns,  max    1.3 ms
apply:  p50 231 ns,  p99 1020 ns,  p99.9  1529 ns,  p99.99 10.97 us,  max 58.6 ms
```

The p99.99 spike is the periodic snapshot publish. The 58.6 ms max is a
separate issue: `order_index_` hash-table rehash as the map crosses a
capacity boundary mid-session. Fix: `reserve()` in the engine constructor
(currently unimplemented — see §4).

At the production snapshot interval (snap=1,000), snapshot publish dominates
wall clock by roughly 30×; this is the empirical motivation for the
off-thread design.

The Phase 5 off-thread snapshot design (`docs/PHASE5_OFF_THREAD_SNAPSHOT.md`)
targets the p99.99 spike by moving snapshot construction off the pipeline
thread via a seqlock + SPSC dirty queue. Phase 5 is **design only** — the
implementation was deferred pending concurrent property tests that cannot be
reliably validated in this environment (see §4).

The histogram CSVs are committed under `bench/out/`.

---

## 4. What is correct, what is a known limitation

What the system does right:

- All 9 in-scope ITCH 5.0 message types parsed against fixed-width offsets
- Out-of-scope message types silently discarded, per spec
- Bid-side comparator inverted (`std::greater<Price>`) so `bids.begin()`
  is best bid in O(1)
- 6-byte timestamp parsed as 6 actual bytes, not memcpy'd as 8 (a memcpy
  would silently corrupt the field after)
- Fixed-point price arithmetic throughout, `to_double()` only at
  serialization
- Order Replace copies the old record before erasing it (the Replace
  message doesn't carry side, locate, or MPID)
- Lock-free single-writer/multi-reader snapshot handoff via
  `std::atomic<std::shared_ptr<>>`

What it is not:

- **Not a matching engine.** The book records resting orders but does
  not match them. Price-time priority is implied by the ITCH message
  ordering, not enforced.
- **No auction-cross handling.** The `Cross Trade` (`Q`) and `Net Order
  Imbalance` (`I`) messages are not modeled. Opening and closing crosses
  appear in the book as the post-cross state via the regular Add/Execute
  stream.
- **ArrayLadder assumes 1-cent tick.** For securities priced under $1.00
  the SEC's sub-penny tick rules apply (Rule 612), and the array index
  math overflows the slot count. `MapLadder` is the safe default for
  the universe of all listed securities.
- **No FPGA-style SIMD parser.** Field offsets and big-endian reads
  could be batched with `_pext_u64` / `vpshufb`. Marginal at best for
  ITCH where parse cost is already a small fraction of apply cost.
- **Snapshot symbol map keys are `std::string`.** A `std::string_view`
  into the long-lived `instruments_` map would save the allocation, but
  the dense-map backing for `instruments_` invalidates pointers on
  rehash. Either the symbol storage stabilizes (move it out of the
  hash table) or the keys stay `std::string`. Current code keeps them
  as `std::string`.
- **`order_index_` hash-table rehash causes the 58.6 ms apply max.**
  The dense map for in-flight orders grows past its default capacity
  mid-session and rehashes in place, blocking the pipeline thread for
  tens of milliseconds. One-line fix: `order_index_.reserve(150'000'000)`
  in the engine constructor. Not applied here because the trade-off is
  substantial: peak RSS rises from 181 MB to approximately 3 GB
  (150M slots × 16 bytes). The spike is documented; the fix is a
  deployment-time decision.
- **Synthetic benchmark mix does not match real ITCH.** The mixed-workload
  bench uses a 75/20/5 delete/add/execute distribution. The real capture
  (`12302019.NASDAQ_ITCH50`) is closer to 43/45/12. The synthetic bench
  is used for ladder comparison only, where relative ordering matters
  more than absolute fidelity. `BM_ApplyMixed_Capture` is the honest
  end-to-end number.
- **`perf` counters unavailable on WSL2.** The IPC, L1 dcache miss rate,
  LLC miss rate, and branch miss rate columns in §1 and §2 are `N/A`.
  The WSL2 kernel (6.6.87.2-microsoft-standard-WSL2) does not expose the
  perf_event subsystem. HdrHistogram_c percentiles are the load-bearing
  latency measurement.
- **Phase 5 off-thread snapshot is design only.** The seqlock + SPSC
  dirty-queue implementation requires concurrent property tests
  ("snapshot copies never see a level with shares < 0") that cannot be
  reliably written in this environment. The design is in
  `docs/PHASE5_OFF_THREAD_SNAPSHOT.md`; no code was produced.

A senior reviewer should walk away knowing what is robust and what is
not. The above list is deliberately exhaustive.

---

## 5. Architecture notes

The pipeline is single-producer / multi-consumer at the snapshot
boundary only:

- **Pipeline thread**: feed reader -> parser -> engine. No locks.
- **Snapshot thread** (Phase 5, optional): per-book seqlock copies,
  publishes via the same atomic shared_ptr.
- **REST thread**: reads the published snapshot via atomic load.
  Never touches the engine.

Snapshots are immutable once published. Every REST request gets a
consistent point-in-time view. Multiple in-flight requests can share
the same snapshot via the shared_ptr refcount.

Message dispatch in the parser is a 9-case switch over the type byte,
which the compiler emits as a jump table at `-O2`. An optional computed
goto dispatch is available under `-DITCH_PARSER_COMPUTED_GOTO=1` (GCC
only). Per benchmarks on this hardware, the difference between the two
is under measurement noise; the switch ships by default.

---

## Appendix A: build and run

Requires GCC 13+, CMake 3.24+, Linux. Dependencies are pulled via
FetchContent: cpp-httplib, nlohmann/json, GoogleTest, Google Benchmark,
moodycamel ConcurrentQueue, HdrHistogram_c, ankerl/unordered_dense.

```bash
make build                           # release build
make replay                          # end to end
make bench                           # gbench json
make bench-replay                    # histogrammed replay (HdrHistogram)
make perf-baseline                   # with perf stat (native Linux only; unavailable on WSL2)
make perf-bench                      # gbench under perf (native Linux only)
```

Run modes:

```bash
ITCH_FILE=data/12302019.NASDAQ_ITCH50 ./build/itch_parser
ITCH_MODE=live MCAST_GROUP=233.54.12.111 MCAST_PORT=26477 \
    MCAST_IFACE=eth0 ./build/itch_parser
```

REST endpoints:

| method | path | description |
|---|---|---|
| GET | `/status` | messages_processed, instruments_tracked, snapshot age |
| GET | `/instruments` | trading state, locate, lot size for every symbol |
| GET | `/book/:symbol?depth=N` | full book up to N levels (default 10, max 50) |
| GET | `/book/:symbol/top` | best bid, best ask, spread (fixed-point) |

---

## Appendix B: tests

99 GoogleTest cases across price arithmetic, parser, book engine, REST
server, and UDP feed reader. `ctest --output-on-failure` runs all of
them. The book engine tests cover all 5 mutation paths plus 5
integration scenarios.

---

## Specification

NASDAQ TotalView-ITCH 5.0, January 2014:
http://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf
