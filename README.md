# ITCH 5.0 Market Data Feed Parser

A C++ limit order book reconstruction system for NASDAQ TotalView-ITCH 5.0 binary feeds. Parses all book-mutating message types, maintains a per-instrument order book in memory, and serves current book state via a REST API. Designed for low-latency throughput with no lock contention on the hot path.

Validated against a real NASDAQ production capture (December 2019, 7.7 GB).

---

## Architecture

```
Binary File
    │
    ▼
┌──────────────────────────────────────────┐
│  Feed Reader                              │
│  next_message() → MessageBuffer           │
└──────────────────┬───────────────────────┘
                   │ raw bytes
                   ▼
┌──────────────────────────────────────────┐
│  Message Parser                           │  PIPELINE THREAD
│  parse() → std::variant<...Msg>           │
└──────────────────┬───────────────────────┘
                   │ typed message variant
                   ▼
┌──────────────────────────────────────────┐
│  Order Book Engine                        │
│  apply message → update book + index      │
│  every N messages → construct snapshot    │
└──────────────────┬───────────────────────┘
                   │ atomic store
                   ▼
        ┌──────────────────────┐
        │   Snapshot Publisher  │
        │  atomic<shared_ptr>   │
        └──────────┬───────────┘
                   │ atomic load (per request)
                   ▼
┌──────────────────────────────────────────┐
│  REST Server (cpp-httplib)                │  REST THREAD
│  load snapshot → read → serialize JSON    │
└──────────────────────────────────────────┘
```

The pipeline is intentionally single-threaded — no lock contention on the hot path. The only synchronization between threads is one atomic pointer swap per snapshot interval. The REST thread always reads a fully consistent, immutable snapshot and never touches live book state.

---

## Key Design Decisions

**Fixed-point price arithmetic** — ITCH encodes prices as `uint32_t` with 4 implied decimal places (`102500` = `$10.2500`). A `Price` struct wraps the raw integer and exposes `to_double()` only at serialization time. All internal comparisons, map keying, and spread computation use the integer representation. Floating-point never appears on the critical path.

**6-byte timestamp parsing** — ITCH timestamps are 6-byte big-endian integers stored in `uint64_t`. The parser reconstructs them with explicit byte shifts. A naive 8-byte `memcpy` at the same offset would silently corrupt the timestamp and the field immediately following it — a spec detail that produces a subtle, hard-to-detect bug.

**Bid-side map comparator** — `std::map<Price, PriceLevel, std::greater<Price>>` for bids, default ascending for asks. `begin()` is always best bid / best ask in O(1). A wrong comparator on the bid side inverts the book silently — it's a one-character difference that must be intentional.

**Lock-free snapshot handoff** — `std::atomic<std::shared_ptr<Snapshot>>` (C++20) gives a lock-free single-writer / multiple-reader handoff. The pipeline thread constructs a new snapshot object each publish cycle and atomically stores the pointer. The REST thread atomically loads it. No mutexes, no condition variables, no reader blocking the pipeline.

**Global order index** — `std::unordered_map<uint64_t, OrderRecord>` keyed by order reference number, shared across all instruments. Every Execute, Cancel, Delete, and Replace message carries only the order reference — without this index, finding the correct price level would require an O(n) search. The index stores side and price so any mutation is O(log n).

**Order Replace semantics** — the Replace message does not carry side, stock locate, or MPID. These must be read from the existing `OrderRecord` before it is erased. Reading after erase is a use-after-free. The implementation explicitly copies the old record first.

---

## Message Types

**Book-mutating:** Add Order (`A`), Add Order with MPID (`F`), Order Executed (`E`), Order Executed with Price (`C`), Order Cancel (`X`), Order Delete (`D`), Order Replace (`U`)

**Informational:** Stock Directory (`R`), Stock Trading Action (`H`)

All other type codes are silently discarded — correct per spec, not an error.

---

## REST API

| Method | Path | Description |
|---|---|---|
| `GET` | `/status` | Messages processed, instruments tracked, snapshot age, pipeline complete |
| `GET` | `/instruments` | All instruments with trading state, locate code, lot size |
| `GET` | `/book/:symbol?depth=N` | Order book up to N price levels per side (default 10, max 50) |
| `GET` | `/book/:symbol/top` | Best bid, best ask, spread (fixed-point computed) |

Symbol lookup is case-insensitive. All endpoints return JSON. Returns 503 before the first snapshot is published.

---

## Building

Requires GCC 13+, CMake 3.24+, Linux. All dependencies fetched automatically via FetchContent (cpp-httplib, nlohmann/json, Google Test, Google Benchmark, moodycamel ConcurrentQueue).

```bash
git clone <repo> && cd itch-parser
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --parallel
ctest --output-on-failure   # 99 tests
```

Release build (required for benchmarks):
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release && cmake --build . --parallel
```

---

## Running

```bash
# Download a sample file from NASDAQ
wget "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz"
gunzip 12302019.NASDAQ_ITCH50.gz
mv 12302019.NASDAQ_ITCH50 data/

# Run (file mode)
ITCH_FILE=data/12302019.NASDAQ_ITCH50 ./itch_parser

# Run (live UDP feed mode)
ITCH_MODE=live MCAST_GROUP=233.54.12.111 MCAST_PORT=26477 MCAST_IFACE=eth0 ./itch_parser
```

```bash
curl http://localhost:8080/status
curl http://localhost:8080/book/AAPL/top
curl "http://localhost:8080/book/AAPL?depth=5"
curl http://localhost:8080/instruments
```

Environment variables: `ITCH_FILE`, `ITCH_MODE` (`file`/`live`), `REST_PORT` (default `8080`), `SNAPSHOT_INTERVAL` (default `1000`), `MCAST_GROUP`, `MCAST_PORT`, `MCAST_IFACE`.

---

## Tests

99 tests, 0 failures across five components:

| File | Coverage |
|---|---|
| `test_price.cpp` | Fixed-point comparison, `to_double()`, spread arithmetic |
| `test_parser.cpp` | Golden-input tests for all 9 message types, every field individually asserted, 6-byte timestamp zero-extension |
| `test_book_engine.cpp` | All 5 mutation operations, 5 integration scenarios, `last_update_timestamp` propagation |
| `test_rest.cpp` | All 4 endpoints, depth limiting, error codes (400/404/503), case-insensitive lookup, pre-snapshot behavior |
| `test_udp_feed_reader.cpp` | MoldUDP64 framing, multi-message packets, gap detection, heartbeat handling, duplicate packet discard |

---

## Benchmarks

Release build (`-O2 -march=native`), Intel i7-14650HX, WSL2:

| Benchmark | Latency | Throughput |
|---|---|---|
| Parse AddOrder | 17.4 ns | 57M msg/s |
| Parse OrderDelete | 4.23 ns | 236M msg/s |
| Parse OrderReplace | 4.58 ns | 218M msg/s |
| Parse unknown type (discard) | 1.31 ns | 766M msg/s |
| Book engine AddOrder | 72.1 ns | 13.9M msg/s |
| Book engine DeleteOrder | 19.1 ns | 52.5M msg/s |
| Book engine ReplaceOrder | 7.41 ns | 135M msg/s |
| Book engine ExecuteOrder | 3.64 ns | 275M msg/s |

`AddOrder` is the most expensive book operation — it inserts into both `std::map` and `std::unordered_map`. `ExecuteOrder` is cheapest after the initial index lookup since partial executions don't remove the price level.

---

## Specification

NASDAQ TotalView-ITCH 5.0 specification (January 2014):
http://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf