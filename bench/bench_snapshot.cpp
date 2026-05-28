// bench/bench_snapshot.cpp
//
// build_snapshot() runs every 1000 messages in production but was never
// benchmarked because BM_* called set_snapshot_interval(UINT64_MAX) to
// disable it. This bench measures it at realistic scales:
//
//   BM_BuildSnapshot/instr/levels   ->  symbols x levels-per-book
//
// Parameter pairs are chosen to bracket the NASDAQ session:
//   - 100 instruments, 20 levels      ~tiny test load
//   - 1000 instruments, 50 levels     ~mid-day for a feed slice
//   - 5000 instruments, 100 levels    ~late-session full TotalView

#include <benchmark/benchmark.h>
#include <string>
#include <vector>

#include "book_engine.h"
#include "snapshot_publisher.h"

namespace {

itch::AddOrderMsg make_add(uint64_t ref, char side, uint32_t shares,
                           uint32_t price_raw, uint16_t locate) {
    itch::AddOrderMsg m;
    m.stock_locate = locate;
    m.order_ref    = ref;
    m.side         = side;
    m.shares       = shares;
    m.price.raw    = price_raw;
    m.timestamp    = 34200000000000ULL;
    return m;
}

std::string symbol_for(uint16_t locate) {
    char buf[8] = {' ',' ',' ',' ',' ',' ',' ',' '};
    int v = locate;
    int i = 7;
    while (v > 0 && i >= 0) {
        buf[i--] = char('A' + (v % 26));
        v /= 26;
    }
    return std::string(buf, 8);
}

void seed_engine(itch::OrderBookEngine& engine, int n_instr, int levels) {
    for (int li = 1; li <= n_instr; ++li) {
        itch::InstrumentInfo info;
        info.stock_locate   = uint16_t(li);
        info.symbol         = symbol_for(uint16_t(li));
        info.trading_state  = 'T';
        info.round_lot_size = 100;
        engine.register_instrument(info);
    }
    engine.set_snapshot_interval(UINT64_MAX);

    uint64_t ref = 1;
    for (int li = 1; li <= n_instr; ++li) {
        for (int l = 0; l < levels; ++l) {
            uint32_t bid = 1000000 + uint32_t(l) * 100;
            uint32_t ask = 1010000 + uint32_t(l) * 100;
            engine.apply(make_add(ref++, 'B', 100, bid, uint16_t(li)));
            engine.apply(make_add(ref++, 'S', 100, ask, uint16_t(li)));
        }
    }
}

} // namespace

static void BM_BuildSnapshot(benchmark::State& state) {
    const int n_instr = int(state.range(0));
    const int levels  = int(state.range(1));

    itch::SnapshotPublisher publisher;
    itch::OrderBookEngine   engine(publisher);
    seed_engine(engine, n_instr, levels);

    for (auto _ : state) {
        auto snap = engine.build_snapshot_for_test();
        benchmark::DoNotOptimize(snap);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["instruments"] = n_instr;
    state.counters["levels_per_side"] = levels;
}
BENCHMARK(BM_BuildSnapshot)
    ->Args({100, 20})
    ->Args({1000, 50})
    ->Args({5000, 100})
    ->Unit(benchmark::kMicrosecond);
