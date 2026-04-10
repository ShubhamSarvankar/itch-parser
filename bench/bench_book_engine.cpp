#include <benchmark/benchmark.h>
#include "book_engine.h"
#include "snapshot_publisher.h"
#include <memory>

// Fixtures — pre-constructed message structs, no parsing in the loop

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

// Seed engine with one instrument and N resting orders
static void seed_engine(itch::OrderBookEngine& engine, int n_orders) {
    itch::InstrumentInfo info;
    info.stock_locate   = 1;
    info.symbol         = "AAPL";
    info.trading_state  = 'T';
    info.round_lot_size = 100;
    engine.register_instrument(info);
    engine.set_snapshot_interval(UINT64_MAX);  // disable snapshot publishing

    for (int i = 0; i < n_orders; ++i) {
        engine.apply(make_add(
            static_cast<uint64_t>(i + 1),
            (i % 2 == 0) ? 'B' : 'S',
            100,
            1000000 + static_cast<uint32_t>(i % 100)
        ));
    }
}

// Benchmarks

static void BM_AddOrder(benchmark::State& state) {
    itch::SnapshotPublisher publisher;
    itch::OrderBookEngine   engine(publisher);
    itch::InstrumentInfo info;
    info.stock_locate = 1; info.symbol = "AAPL";
    info.trading_state = 'T'; info.round_lot_size = 100;
    engine.register_instrument(info);
    engine.set_snapshot_interval(UINT64_MAX);

    uint64_t ref = 1;
    for (auto _ : state) {
        engine.apply(make_add(ref++, 'B', 100, 1000000));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AddOrder);

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
    int      idx      = 0;
    int      n        = static_cast<int>(state.range(0));

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

    // Partial execute (50 of 100 shares) so orders stay alive
    int   idx = 0;
    int   n   = static_cast<int>(state.range(0));
    for (auto _ : state) {
        uint64_t ref = static_cast<uint64_t>((idx % n) + 1);
        engine.apply(make_execute(ref, 1));
        ++idx;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ExecuteOrder)->Arg(10000);
