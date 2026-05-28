// bench/bench_ladder_shootout.cpp
//
// Apples-to-apples microbenchmarks for the three PriceLadder implementations.
//
// Workload (identical for all three):
//   1. Seed with N "live" orders, prices drawn from a log-normal centered
//      at mid_raw, capped within +/- 4*spread_raw of mid. Same seed = same
//      sequence across runs and across ladders.
//   2. Replay 100k 75/20/5 delete/add/execute steps over that book.
//
// For ArrayLadder, base_raw is set to (mid_raw - 4*spread_raw) and SLOTS
// is sized to cover the full band at the equity tick of 100 raw units.
//
// Reports gbench mean per-op; pair with --benchmark_repetitions=10
// --benchmark_report_aggregates_only=true for p50 / median / stddev.

#include <benchmark/benchmark.h>
#include <random>
#include <vector>

#include "itch/price_ladder.h"

namespace {

constexpr uint32_t MID    = 1'000'000;
constexpr uint32_t SPREAD = 2500;
constexpr uint32_t BASE   = MID - 4 * SPREAD;
constexpr uint32_t SLOTS  = 1024;  // covers MID +/- ~5 * SPREAD

enum Op : uint8_t { OP_ADD, OP_REMOVE_PARTIAL, OP_REMOVE_FULL };

struct Step {
    Op       op;
    uint32_t price_raw;
    uint32_t shares;
};

std::vector<Step> build_workload(int n_seed, int n_replay, uint32_t seed) {
    std::mt19937 rng(seed);
    std::lognormal_distribution<double> ln(0.0, 0.6);
    std::vector<Step> out;
    out.reserve(n_seed + n_replay);

    auto sample_price = [&]() {
        double s = ln(rng);
        double signed_offset = ((rng() & 1) ? 1.0 : -1.0) * s * SPREAD;
        int64_t p = int64_t(MID) + int64_t(signed_offset);
        if (p < int64_t(BASE)) p = BASE;
        if (p > int64_t(BASE) + int64_t(SLOTS - 1) * 100)
            p = BASE + int64_t(SLOTS - 1) * 100;
        // Snap to tick.
        return uint32_t(p / 100 * 100);
    };

    // Track live orders to drive realistic removes.
    std::vector<uint32_t> live_prices;
    live_prices.reserve(n_seed + n_replay);

    for (int i = 0; i < n_seed; ++i) {
        uint32_t p = sample_price();
        out.push_back({OP_ADD, p, 100});
        live_prices.push_back(p);
    }

    std::uniform_int_distribution<int> mix(0, 99);
    for (int i = 0; i < n_replay; ++i) {
        int r = mix(rng);
        if (r < 75 && !live_prices.empty()) {
            std::uniform_int_distribution<size_t> pick(0, live_prices.size() - 1);
            size_t idx = pick(rng);
            uint32_t p = live_prices[idx];
            live_prices[idx] = live_prices.back();
            live_prices.pop_back();
            out.push_back({OP_REMOVE_FULL, p, 100});
        } else if (r < 95 || live_prices.empty()) {
            uint32_t p = sample_price();
            out.push_back({OP_ADD, p, 100});
            live_prices.push_back(p);
        } else {
            std::uniform_int_distribution<size_t> pick(0, live_prices.size() - 1);
            uint32_t p = live_prices[pick(rng)];
            out.push_back({OP_REMOVE_PARTIAL, p, 1});
        }
    }
    return out;
}

template <class Ladder>
void run_workload_inner(Ladder& ladder, const std::vector<Step>& ops,
                        std::size_t& idx) {
    const Step& s = ops[idx];
    switch (s.op) {
        case OP_ADD:
            ladder.add_shares(itch::Price{s.price_raw}, s.shares);
            break;
        case OP_REMOVE_PARTIAL:
            ladder.remove_shares(itch::Price{s.price_raw}, s.shares, false);
            break;
        case OP_REMOVE_FULL:
            ladder.remove_shares(itch::Price{s.price_raw}, s.shares, true);
            break;
    }
    idx = (idx + 1 == ops.size()) ? 0 : idx + 1;
}

} // namespace

// --- Bid-side benches ------------------------------------------------------

static void BM_Ladder_Map_Bid(benchmark::State& state) {
    auto ops = build_workload(int(state.range(0)), 100000, 0xC0FFEE);
    itch::MapLadder<itch::Side::Bid> ladder;
    // Seed inline so the steady-state mix dominates measurement.
    std::size_t idx = 0;
    for (int i = 0; i < int(state.range(0)); ++i) {
        run_workload_inner(ladder, ops, idx);
    }
    for (auto _ : state) {
        run_workload_inner(ladder, ops, idx);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["levels"] = double(ladder.size());
}
BENCHMARK(BM_Ladder_Map_Bid)->Arg(10000)->Unit(benchmark::kNanosecond);

static void BM_Ladder_Flat_Bid(benchmark::State& state) {
    auto ops = build_workload(int(state.range(0)), 100000, 0xC0FFEE);
    itch::FlatLadder<itch::Side::Bid> ladder;
    std::size_t idx = 0;
    for (int i = 0; i < int(state.range(0)); ++i) {
        run_workload_inner(ladder, ops, idx);
    }
    for (auto _ : state) {
        run_workload_inner(ladder, ops, idx);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["levels"] = double(ladder.size());
}
BENCHMARK(BM_Ladder_Flat_Bid)->Arg(10000)->Unit(benchmark::kNanosecond);

static void BM_Ladder_Array_Bid(benchmark::State& state) {
    auto ops = build_workload(int(state.range(0)), 100000, 0xC0FFEE);
    itch::ArrayLadder<itch::Side::Bid, SLOTS, 100> ladder(BASE);
    std::size_t idx = 0;
    for (int i = 0; i < int(state.range(0)); ++i) {
        run_workload_inner(ladder, ops, idx);
    }
    for (auto _ : state) {
        run_workload_inner(ladder, ops, idx);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["levels"] = double(ladder.size());
}
BENCHMARK(BM_Ladder_Array_Bid)->Arg(10000)->Unit(benchmark::kNanosecond);

// --- Top-of-book read latency ---------------------------------------------

template <class Ladder>
static void run_top_bench(benchmark::State& state, Ladder& ladder) {
    for (auto _ : state) {
        auto* b = ladder.best();
        benchmark::DoNotOptimize(b);
    }
    state.SetItemsProcessed(state.iterations());
}

static void BM_Ladder_Map_Top(benchmark::State& state) {
    auto ops = build_workload(int(state.range(0)), 100000, 0xC0FFEE);
    itch::MapLadder<itch::Side::Bid> ladder;
    std::size_t idx = 0;
    for (size_t i = 0; i < ops.size(); ++i) run_workload_inner(ladder, ops, idx);
    run_top_bench(state, ladder);
}
BENCHMARK(BM_Ladder_Map_Top)->Arg(10000)->Unit(benchmark::kNanosecond);

static void BM_Ladder_Flat_Top(benchmark::State& state) {
    auto ops = build_workload(int(state.range(0)), 100000, 0xC0FFEE);
    itch::FlatLadder<itch::Side::Bid> ladder;
    std::size_t idx = 0;
    for (size_t i = 0; i < ops.size(); ++i) run_workload_inner(ladder, ops, idx);
    run_top_bench(state, ladder);
}
BENCHMARK(BM_Ladder_Flat_Top)->Arg(10000)->Unit(benchmark::kNanosecond);

static void BM_Ladder_Array_Top(benchmark::State& state) {
    auto ops = build_workload(int(state.range(0)), 100000, 0xC0FFEE);
    itch::ArrayLadder<itch::Side::Bid, SLOTS, 100> ladder(BASE);
    std::size_t idx = 0;
    for (size_t i = 0; i < ops.size(); ++i) run_workload_inner(ladder, ops, idx);
    run_top_bench(state, ladder);
}
BENCHMARK(BM_Ladder_Array_Top)->Arg(10000)->Unit(benchmark::kNanosecond);
