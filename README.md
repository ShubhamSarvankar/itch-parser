# ITCH 5.0 Market Data Feed Parser

A C++ order book reconstruction system for NASDAQ TotalView-ITCH 5.0 binary feeds. Parses every book-mutating message type, maintains a per-instrument limit order book in memory, and serves current book state via a REST API.

---

## What it does

- Reads a raw ITCH 5.0 binary file, parsing length-prefixed message frames
- Reconstructs a full limit order book per instrument (Add, Execute, Cancel, Delete, Replace)
- Resolves stock locate codes to ticker symbols via Stock Directory messages
- Publishes immutable snapshots atomically on a configurable interval
- Serves live book state over HTTP while the pipeline is running

Validated against a real NASDAQ production capture (December 2019, ~70M messages).

---

## Architecture

```
Binary File
    │
    ▼
┌─────────────────┐
│   Feed Reader   │  Frames raw bytes, detects truncation and EOF
└────────┬────────┘
         │ MessageBuffer
         ▼
┌─────────────────┐
│  Message Parser │  PIPELINE THREAD — stateless, big-endian deserializer
└────────┬────────┘
         │ ParsedMessage (std::variant)
         ▼
┌─────────────────┐
│  Book Engine    │  order_index + per-instrument OrderBook + instrument registry
└────────┬────────┘
         │ atomic store (every N messages)
         ▼
┌─────────────────┐
│    Snapshot     │  std::atomic<std::shared_ptr<SystemSnapshot>>
└────────┬────────┘
         │ atomic load (per request)
         ▼
┌─────────────────┐
│  REST Server    │  REST THREAD — cpp-httplib, read-only, no live book access
└─────────────────┘
```

The pipeline is single-threaded by design — no lock contention on the hot path. The only shared state between threads is one atomic pointer swap per snapshot interval.

---

## Design decisions worth noting

**Fixed-point price** — ITCH encodes prices as `uint32_t` with 4 implied decimal places. A `Price` struct wraps the raw value and exposes `to_double()` only at serialization time. All internal comparisons and map keying use the integer representation — no floating-point arithmetic on the critical path.

**6-byte timestamp** — ITCH timestamps are 6-byte big-endian integers. The parser reconstructs them with explicit byte shifts into `uint64_t`. A naive 8-byte `memcpy` at the same offset would silently corrupt the timestamp and the following field.

**Bid-side map comparator** — `std::map<Price, PriceLevel, std::greater<Price>>` for bids, default ascending for asks. `begin()` is always best bid / best ask. Top-of-book is O(1).

**Atomic snapshot handoff** — `std::atomic<std::shared_ptr<T>>` (C++20) gives a lock-free single-writer / multiple-reader handoff. The REST thread always reads a fully consistent snapshot — never a partially-written state.

---

## Building

Requires GCC 13+, CMake 3.24+, Linux. All dependencies fetched automatically via FetchContent.

```bash
git clone <repo> && cd itch-parser
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --parallel
ctest --output-on-failure
```

---

## Running

```bash
# Download a sample file from NASDAQ
wget "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz"
gunzip 12302019.NASDAQ_ITCH50.gz

# Run
ITCH_FILE=data/12302019.NASDAQ_ITCH50 SNAPSHOT_INTERVAL=100000 ./itch_parser
```

```bash
curl http://localhost:8080/status
curl http://localhost:8080/book/AAPL/top
curl "http://localhost:8080/book/AAPL?depth=5"
curl http://localhost:8080/instruments
```

---

## Tests

70 tests across three components:

- **Price** — comparison operators, fixed-point conversion, spread arithmetic
- **Parser** — golden-input tests for all 9 in-scope message types, every field individually asserted, 6-byte timestamp zero-extension verified
- **Book engine** — unit tests for all 5 mutation operations + 5 integration scenarios (partial fill, replace, full execution, multi-order level, mid-session start)

---

## Benchmarks

Release build (`-O2 -march=native`), GCC 13.3, WSL2:

| Benchmark | Time | Throughput |
|---|---|---|
| Parse AddOrder | 17.5 ns | 57M msg/s |
| Parse OrderDelete | 4.07 ns | 246M msg/s |
| Parse OrderReplace | 4.40 ns | 227M msg/s |
| Parse unknown type | 1.30 ns | 770M msg/s |
| Book engine AddOrder | 66.8 ns | 15M msg/s |
| Book engine DeleteOrder | 19.3 ns | 52M msg/s |
| Book engine ReplaceOrder | 7.14 ns | 140M msg/s |
| Book engine ExecuteOrder | 3.61 ns | 277M msg/s |

---

## Spec

NASDAQ TotalView-ITCH 5.0:
http://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf