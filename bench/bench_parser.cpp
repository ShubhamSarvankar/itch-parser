#include <benchmark/benchmark.h>
#include <vector>
#include <cstdint>
#include "parser.h"

// Buffer construction helpers (mirrors test_parser.cpp write_be* helpers)

static void write_be16(std::vector<uint8_t>& b, size_t o, uint16_t v) {
    b[o] = (v>>8)&0xFF; b[o+1] = v&0xFF;
}
static void write_be32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    b[o]=(v>>24)&0xFF; b[o+1]=(v>>16)&0xFF; b[o+2]=(v>>8)&0xFF; b[o+3]=v&0xFF;
}
static void write_be48(std::vector<uint8_t>& b, size_t o, uint64_t v) {
    b[o]=(v>>40)&0xFF; b[o+1]=(v>>32)&0xFF; b[o+2]=(v>>24)&0xFF;
    b[o+3]=(v>>16)&0xFF; b[o+4]=(v>>8)&0xFF; b[o+5]=v&0xFF;
}
static void write_be64(std::vector<uint8_t>& b, size_t o, uint64_t v) {
    b[o]=(v>>56)&0xFF; b[o+1]=(v>>48)&0xFF; b[o+2]=(v>>40)&0xFF;
    b[o+3]=(v>>32)&0xFF; b[o+4]=(v>>24)&0xFF; b[o+5]=(v>>16)&0xFF;
    b[o+6]=(v>>8)&0xFF; b[o+7]=v&0xFF;
}

static itch::MessageBuffer make_add_order() {
    itch::MessageBuffer buf(36, 0);
    buf[0] = 'A';
    write_be16(buf,  1, 1);
    write_be16(buf,  3, 0);
    write_be48(buf,  5, 34200000000000ULL);
    write_be64(buf, 11, 123456789ULL);
    buf[19] = 'B';
    write_be32(buf, 20, 100);
    buf[24]='A'; buf[25]='A'; buf[26]='P'; buf[27]='L';
    for (int i = 28; i < 32; ++i) buf[i] = ' ';
    write_be32(buf, 32, 1894200);
    return buf;
}

static itch::MessageBuffer make_order_delete() {
    itch::MessageBuffer buf(19, 0);
    buf[0] = 'D';
    write_be16(buf,  1, 1);
    write_be16(buf,  3, 0);
    write_be48(buf,  5, 34200000000001ULL);
    write_be64(buf, 11, 123456789ULL);
    return buf;
}

static itch::MessageBuffer make_order_replace() {
    itch::MessageBuffer buf(35, 0);
    buf[0] = 'U';
    write_be16(buf,  1, 1);
    write_be16(buf,  3, 0);
    write_be48(buf,  5, 34200000000002ULL);
    write_be64(buf, 11, 111111111ULL);
    write_be64(buf, 19, 222222222ULL);
    write_be32(buf, 27, 200);
    write_be32(buf, 31, 1894300);
    return buf;
}

// Benchmarks — no I/O in the benchmark loop, buffers pre-constructed

static void BM_ParseAddOrder(benchmark::State& state) {
    itch::MessageParser parser;
    auto buf = make_add_order();
    for (auto _ : state) {
        auto result = parser.parse(buf);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseAddOrder);

static void BM_ParseOrderDelete(benchmark::State& state) {
    itch::MessageParser parser;
    auto buf = make_order_delete();
    for (auto _ : state) {
        auto result = parser.parse(buf);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseOrderDelete);

static void BM_ParseOrderReplace(benchmark::State& state) {
    itch::MessageParser parser;
    auto buf = make_order_replace();
    for (auto _ : state) {
        auto result = parser.parse(buf);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseOrderReplace);

static void BM_ParseUnknownType(benchmark::State& state) {
    itch::MessageParser parser;
    itch::MessageBuffer buf(10, 0);
    buf[0] = 0xFF;
    for (auto _ : state) {
        auto result = parser.parse(buf);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseUnknownType);
