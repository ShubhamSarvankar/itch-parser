# ITCH 5.0 Order Book Reconstruction

A NASDAQ TotalView-ITCH 5.0 binary feed parser and in-memory order book
maintained in C++20. Built against a real production capture
(`12302019.NASDAQ_ITCH50`, 7.7 GB, 412M messages), with a focus on
honest latency measurement rather than headline microbenchmark numbers.

---

## 1. Headline

End to end replay of the December 2019 NASDAQ capture, no tuning, no
trimming, single threaded, snapshot publish every 1000 messages
included in the cost:

| metric                              | value     |
|-------------------------------------|-----------|
| messages                            | __ M      |
| wall clock                          | __ s      |
| sustained throughput                | __ M msg/s|
| p50 apply latency                   | __ ns     |
| p99 apply latency                   | __ ns     |
| p99.9 apply latency                 | __ us     |
| peak RSS                            | __ MB     |
| IPC                                 | __        |
| L1 dcache miss rate                 | __ %      |
| LLC miss rate                       | __ %      |

Reproduce with:

```
make perf-baseline ITCH_FILE=data/12302019.NASDAQ_ITCH50
```

Raw `perf stat` output is committed under `bench/out/baseline.perf`.

The interesting story is not the means. It is what changed from the
baseline, and what didn't.

---

## 2. Data structure shootout: how to back the price ladder

The order book's per-side price ladder is the hottest data structure in
the system. Every Add, Delete, Cancel, Execute, and Replace touches it.
Three reasonable choices, all implemented (`include/itch/price_ladder.h`)
and benched (`bench_ladder_shootout.cpp`):

| ladder       | add p50 | add p99 | erase p50 | top()   | bytes/instr  | L1 miss |
|--------------|---------|---------|-----------|---------|--------------|---------|
| `MapLadder`  | __ ns   | __ ns   | __ ns     | __ ns   | __           | __ %    |
| `FlatLadder` | __ ns   | __ ns   | __ ns     | __ ns   | __           | __ %    |
| `ArrayLadder`| __ ns   | __ ns   | __ ns     | __ ns   | __           | __ %    |

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

The snapshot publish that runs every 1000 messages used to run
synchronously on the pipeline thread. Histogram before:

```
parse:     p50 __ ns,  p99 __ ns,  p99.9 __ ns
apply:     p50 __ ns,  p99 __ ns,  p99.9 __ us    <-- snapshot spike
snapshot:  p50 __ us,  p99 __ us,  p99.9 __ us
```

Moving snapshot construction to a dedicated thread with per-book
seqlocks and a SPSC dirty queue (`docs/PHASE5_OFF_THREAD_SNAPSHOT.md`)
collapses the apply tail without affecting the parse path:

```
apply (after): p50 __ ns,  p99 __ ns,  p99.9 __ ns
```

Cost: the seqlock adds two relaxed atomic stores around every book
mutation, measured at __ ns per apply. Trade made: a deterministic small
fixed cost on every message in exchange for removing the periodic
hundreds-of-microseconds spike.

The histogram CSVs that produce this section live in `bench/out/`.

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
make perf-baseline                   # with perf stat
make bench                           # gbench json
make bench-replay                    # histogrammed replay
make perf-bench                      # gbench under perf
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
