// bench/bench_book_engine.cpp
//
// Targeted micro benchmarks for delete, replace, execute. Seeded with N
// resting orders across 100 distinct prices.
//
// NOTE: the original BM_AddOrder in this file was misleading: it always
// added at the same price, so after the first insert every call was just
// ++level.total_shares. The std::map insert was not measured. That bench
// is replaced by BM_AddOrder_HotBand / BM_AddOrder_ColdPrice in
// bench_mixed_workload.cpp.
//
// For honest end-to-end numbers see BM_ApplyMixed_Synth /
// BM_ApplyMixed_Capture in bench_mixed_workload.cpp.

#include <benchmark/benchmark.h>
#include <memory>
#include "book_engine.h"
#include "snapshot_publisher.h"

static itch::AddOrderMsg make_add(uint64_t ref, char side,
                                   uint32_t shares, uint32_t price_raw) {
    itch::AddOrderMsg m;
    m.stock_locate = 1;
    m.order_ref    = ref;
    m.side         = side;
    m.shares       = shares;
    m.price.raw    = price_raw;
    m.timestamp    = 34200000000000ULL;
    return m;
}

static itch::OrderDeleteMsg make_delete(uint64_t ref) {
    itch::OrderDeleteMsg m;
    m.stock_locate = 1;
    m.order_ref    = ref;
    m.timestamp    = 34200000000001ULL;
    return m;
}

static itch::OrderReplaceMsg make_replace(uint64_t orig, uint64_t next,
                                           uint32_t shares, uint32_t price) {
    itch::OrderReplaceMsg m;
    m.stock_locate       = 1;
    m.original_order_ref = orig;
    m.new_order_ref      = next;
    m.shares             = shares;
    m.price.raw          = price;
    m.timestamp          = 34200000000002ULL;
    return m;
}

static itch::OrderExecutedMsg make_execute(uint64_t ref, uint32_t shares) {
    itch::OrderExecutedMsg m;
    m.stock_locate    = 1;
    m.order_ref       = ref;
    m.executed_shares = shares;
    m.timestamp       = 34200000000003ULL;
    return m;
}

static void seed_engine(itch::OrderBookEngine& engine, int n_orders) {
    itch::InstrumentInfo info;
    info.stock_locate   = 1;
    info.symbol         = "AAPL";
    info.trading_state  = 'T';
    info.round_lot_size = 100;
    engine.register_instrument(info);
    engine.set_snapshot_interval(UINT64_MAX);

    for (int i = 0; i < n_orders; ++i) {
        engine.apply(make_add(
            static_cast<uint64_t>(i + 1),
            (i % 2 == 0) ? 'B' : 'S',
            100,
            1000000 + static_cast<uint32_t>(i % 100)
        ));
    }
}

static void BM_DeleteOrder(benchmark::State& state) {
    int n = static_cast<int>(state.range(0));
    itch::SnapshotPublisher publisher;
    auto engine = std::make_unique<itch::OrderBookEngine>(publisher);
    seed_engine(*engine, n);

    int idx = 0;
    for (auto _ : state) {
        if (idx >= n) {
            state.PauseTiming();
            engine = std::make_unique<itch::OrderBookEngine>(publisher);
            seed_engine(*engine, n);
            idx = 0;
            state.ResumeTiming();
        }
        engine->apply(make_delete(static_cast<uint64_t>(idx + 1)));
        ++idx;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DeleteOrder)->Arg(10000);

static void BM_ReplaceOrder(benchmark::State& state) {
    itch::SnapshotPublisher publisher;
    itch::OrderBookEngine   engine(publisher);
    seed_engine(engine, static_cast<int>(state.range(0)));

    uint64_t next_ref = static_cast<uint64_t>(state.range(0)) + 1;
    int idx = 0;
    int n   = static_cast<int>(state.range(0));
    for (auto _ : state) {
        uint64_t orig = static_cast<uint64_t>((idx % n) + 1);
        engine.apply(make_replace(orig, next_ref++, 200, 1000050));
        ++idx;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ReplaceOrder)->Arg(10000);

static void BM_ExecuteOrder(benchmark::State& state) {
    itch::SnapshotPublisher publisher;
    itch::OrderBookEngine   engine(publisher);
    seed_engine(engine, static_cast<int>(state.range(0)));

    int idx = 0;
    int n   = static_cast<int>(state.range(0));
    for (auto _ : state) {
        uint64_t ref = static_cast<uint64_t>((idx % n) + 1);
        engine.apply(make_execute(ref, 1));
        ++idx;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ExecuteOrder)->Arg(10000);
