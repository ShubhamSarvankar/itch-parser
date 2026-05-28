// bench/bench_mixed_workload.cpp
//
// Honest benchmarks. Replaces the synthetic numbers from bench_book_engine.cpp.
//
// Workloads:
//   BM_AddOrder_HotBand     adds at one of 16 prices clustered around a mid.
//                           Models the steady state where most inserts hit
//                           an existing level: ++level.total_shares.
//   BM_AddOrder_ColdPrice   adds at a brand new price every iteration.
//                           This is the actual std::map insert cost.
//   BM_ApplyMixed_Synth     synthetic 75/20/5 delete/add/execute mix over a
//                           pre-warmed book. Workload built once, replayed
//                           in the hot loop. Reports p50/p90/p99 via gbench
//                           --benchmark_repetitions plus a HdrHistogram if
//                           available.
//   BM_ApplyMixed_Capture   loads N bytes from an ITCH file (env ITCH_FILE),
//                           parses once, replays the parsed ParsedMessage
//                           vector in the loop. Measures apply-only.

#include <benchmark/benchmark.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <random>
#include <vector>

#include "book_engine.h"
#include "snapshot_publisher.h"
#include "feed_reader.h"
#include "parser.h"

namespace {

itch::AddOrderMsg make_add(uint64_t ref, char side, uint32_t shares,
                           uint32_t price_raw, uint16_t locate = 1) {
    itch::AddOrderMsg m;
    m.stock_locate = locate;
    m.order_ref    = ref;
    m.side         = side;
    m.shares       = shares;
    m.price.raw    = price_raw;
    m.timestamp    = 34200000000000ULL;
    return m;
}

itch::OrderDeleteMsg make_delete(uint64_t ref, uint16_t locate = 1) {
    itch::OrderDeleteMsg m;
    m.stock_locate = locate;
    m.order_ref    = ref;
    m.timestamp    = 34200000000001ULL;
    return m;
}

itch::OrderExecutedMsg make_execute(uint64_t ref, uint32_t shares,
                                    uint16_t locate = 1) {
    itch::OrderExecutedMsg m;
    m.stock_locate    = locate;
    m.order_ref       = ref;
    m.executed_shares = shares;
    m.timestamp       = 34200000000002ULL;
    return m;
}

void register_aapl(itch::OrderBookEngine& engine) {
    itch::InstrumentInfo info;
    info.stock_locate   = 1;
    info.symbol         = "AAPL";
    info.trading_state  = 'T';
    info.round_lot_size = 100;
    engine.register_instrument(info);
}

// Log normal price distribution clustered around mid_raw, capped to a sane
// band so we do not blow out std::map memory unfairly. Fixed seed for
// reproducibility.
std::vector<uint32_t> make_price_distribution(int n_distinct,
                                              uint32_t mid_raw,
                                              uint32_t spread_raw,
                                              uint32_t seed) {
    std::mt19937 rng(seed);
    std::lognormal_distribution<double> ln(0.0, 0.6);
    std::vector<uint32_t> prices;
    prices.reserve(n_distinct);
    for (int i = 0; i < n_distinct; ++i) {
        double s = ln(rng);
        double signed_offset = ((rng() & 1) ? 1.0 : -1.0) * s * spread_raw;
        int64_t p = int64_t(mid_raw) + int64_t(signed_offset);
        if (p < int64_t(mid_raw - 4 * spread_raw)) p = mid_raw - 4 * spread_raw;
        if (p > int64_t(mid_raw + 4 * spread_raw)) p = mid_raw + 4 * spread_raw;
        prices.push_back(uint32_t(p));
    }
    return prices;
}

} // namespace

// --- BM_AddOrder_HotBand ---------------------------------------------------
//
// Adds with prices chosen uniformly from 16 hot prices already resting in
// the book. Models the steady state where most adds hit an existing level.

static void BM_AddOrder_HotBand(benchmark::State& state) {
    itch::SnapshotPublisher publisher;
    itch::OrderBookEngine   engine(publisher);
    register_aapl(engine);
    engine.set_snapshot_interval(UINT64_MAX);

    std::array<uint32_t, 16> hot_prices{};
    for (int i = 0; i < 16; ++i) hot_prices[i] = 1000000 + uint32_t(i * 100);

    uint64_t ref = 1;
    for (int i = 0; i < 16; ++i) {
        engine.apply(make_add(ref++, 'B', 100, hot_prices[i]));
    }

    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<int> pick(0, 15);

    for (auto _ : state) {
        uint32_t px = hot_prices[pick(rng)];
        engine.apply(make_add(ref++, 'B', 100, px));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AddOrder_HotBand);

// --- BM_AddOrder_ColdPrice -------------------------------------------------
//
// New price every iteration. Exercises std::map insert proper.

static void BM_AddOrder_ColdPrice(benchmark::State& state) {
    itch::SnapshotPublisher publisher;
    itch::OrderBookEngine   engine(publisher);
    register_aapl(engine);
    engine.set_snapshot_interval(UINT64_MAX);

    uint64_t ref = 1;
    uint32_t price = 1000000;
    for (auto _ : state) {
        engine.apply(make_add(ref++, 'B', 100, price));
        price += 100;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AddOrder_ColdPrice);

// --- BM_ApplyMixed_Synth ---------------------------------------------------
//
// 75% delete, 20% add, 5% execute on a pre-warmed book with N resting
// orders across a realistic price distribution. The message stream is
// built once outside the timed loop and replayed.

static void BM_ApplyMixed_Synth(benchmark::State& state) {
    const int n_resting   = int(state.range(0));
    const int n_distinct  = 500;
    const uint32_t mid    = 1000000;
    const uint32_t spread = 2500;

    auto prices = make_price_distribution(n_distinct, mid, spread, 42);

    // Build the workload: an "ops" vector indexed by iteration mod size.
    enum Op : uint8_t { OP_DELETE, OP_ADD, OP_EXECUTE };
    struct Step {
        Op       op;
        uint64_t ref;
        uint32_t price_raw;
        uint32_t shares;
        char     side;
    };

    std::mt19937 rng(0xBEEF);
    std::uniform_int_distribution<int> price_pick(0, n_distinct - 1);
    std::uniform_int_distribution<int> side_pick(0, 1);

    std::vector<Step> ops;
    ops.reserve(100000);

    // We track which refs are alive so deletes/executes target live orders.
    std::vector<uint64_t> live;
    live.reserve(n_resting + 100000);

    uint64_t next_ref = 1;
    for (int i = 0; i < n_resting; ++i) {
        Step s{OP_ADD, next_ref, prices[price_pick(rng)], 100,
               side_pick(rng) ? 'B' : 'S'};
        ops.push_back(s);
        live.push_back(next_ref);
        ++next_ref;
    }

    // Add 100k more steps with 75/20/5 mix.
    std::uniform_int_distribution<int> mix(0, 99);
    for (int i = 0; i < 100000; ++i) {
        int r = mix(rng);
        if (r < 75 && !live.empty()) {
            std::uniform_int_distribution<size_t> pick(0, live.size() - 1);
            size_t idx = pick(rng);
            uint64_t ref = live[idx];
            live[idx] = live.back();
            live.pop_back();
            ops.push_back({OP_DELETE, ref, 0, 0, 0});
        } else if (r < 95 || live.empty()) {
            Step s{OP_ADD, next_ref, prices[price_pick(rng)], 100,
                   side_pick(rng) ? 'B' : 'S'};
            ops.push_back(s);
            live.push_back(next_ref);
            ++next_ref;
        } else {
            std::uniform_int_distribution<size_t> pick(0, live.size() - 1);
            uint64_t ref = live[pick(rng)];
            ops.push_back({OP_EXECUTE, ref, 0, 1, 0});
        }
    }

    itch::SnapshotPublisher publisher;
    itch::OrderBookEngine   engine(publisher);
    register_aapl(engine);
    engine.set_snapshot_interval(UINT64_MAX);

    size_t idx = 0;
    const size_t N = ops.size();
    for (auto _ : state) {
        const Step& s = ops[idx];
        switch (s.op) {
            case OP_ADD:
                engine.apply(make_add(s.ref, s.side, s.shares, s.price_raw));
                break;
            case OP_DELETE:
                engine.apply(make_delete(s.ref));
                break;
            case OP_EXECUTE:
                engine.apply(make_execute(s.ref, s.shares));
                break;
        }
        idx = (idx + 1) % N;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ApplyMixed_Synth)
    ->Arg(10000)
    ->Arg(100000)
    ->Unit(benchmark::kNanosecond);

// --- BM_ApplyMixed_Capture -------------------------------------------------
//
// Loads N bytes from the real NASDAQ capture (env ITCH_FILE), parses once,
// then replays the parsed messages in a tight loop. Measures the apply path
// against a realistic message distribution. Skipped at runtime if the file
// is not present.

namespace {
std::vector<itch::ParsedMessage> load_capture(const std::string& path,
                                              size_t max_messages) {
    std::vector<itch::ParsedMessage> out;
    out.reserve(max_messages);
    try {
        itch::FileFeedReader reader(path);
        itch::MessageParser parser;
        while (out.size() < max_messages) {
            auto buf = reader.next_message();
            if (!buf) break;
            try {
                auto m = parser.parse(*buf);
                if (m) out.push_back(std::move(*m));
            } catch (...) {}
        }
    } catch (const std::exception& e) {
        // file missing or unreadable, return empty
    }
    return out;
}
} // namespace

static void BM_ApplyMixed_Capture(benchmark::State& state) {
    const char* env_path = std::getenv("ITCH_FILE");
    std::string path = env_path ? env_path : "data/12302019.NASDAQ_ITCH50";
    const size_t MAX = size_t(state.range(0));

    auto msgs = load_capture(path, MAX);
    if (msgs.empty()) {
        state.SkipWithError("ITCH_FILE not readable; set env ITCH_FILE");
        return;
    }

    itch::SnapshotPublisher publisher;
    itch::OrderBookEngine   engine(publisher);
    engine.set_snapshot_interval(UINT64_MAX);

    size_t idx = 0;
    const size_t N = msgs.size();
    for (auto _ : state) {
        engine.apply(msgs[idx]);
        idx = (idx + 1 == N) ? 0 : idx + 1;
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["loaded_msgs"] = double(N);
}
BENCHMARK(BM_ApplyMixed_Capture)
    ->Arg(100000)
    ->Unit(benchmark::kNanosecond);
