// bench/bench_replay.cpp
//
// End-to-end replay of an ITCH 5.0 capture, single threaded, no REST.
//
// Reports: wall clock, sustained msg/sec, peak RSS, and per-type counts.
// Optionally records HDR histograms for parse / apply / snapshot when
// ITCH_PROFILE=1 (see Phase 1).
//
// Usage: bench_replay <path-to-itch-file> [snapshot_interval]

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/resource.h>

#include "feed_reader.h"
#include "parser.h"
#include "book_engine.h"
#include "snapshot_publisher.h"

#ifdef ITCH_HAVE_HDR
#include "bench/hdr_recorder.h"
#endif

namespace {

uint64_t now_ns() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        clock::now().time_since_epoch()).count();
}

long peak_rss_kb() {
    struct rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return r.ru_maxrss;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <itch-file> [snapshot_interval]\n", argv[0]);
        return 2;
    }
    const std::string path = argv[1];
    const uint64_t snapshot_interval =
        (argc >= 3) ? std::strtoull(argv[2], nullptr, 10) : 1000ULL;

    const bool profile = std::getenv("ITCH_PROFILE") != nullptr;

    itch::SnapshotPublisher publisher;
    itch::OrderBookEngine   engine(publisher);
    engine.set_snapshot_interval(snapshot_interval);

#ifdef ITCH_HAVE_HDR
    itch::bench::HdrRecorder hdr;
    if (profile) hdr.enable();
#endif

    uint64_t parse_errors = 0;
    uint64_t messages = 0;
    uint64_t type_counts[256] = {0};

    std::cout << "[bench_replay] file=" << path
              << " snapshot_interval=" << snapshot_interval
              << " profile=" << (profile ? 1 : 0) << "\n";

    const uint64_t t0 = now_ns();

    try {
        itch::FileFeedReader reader(path);
        itch::MessageParser  parser;

        while (auto buf = reader.next_message()) {
            uint8_t type_byte = (*buf).empty() ? 0 : (*buf)[0];
            type_counts[type_byte]++;

#ifdef ITCH_HAVE_HDR
            uint64_t t_parse_start = profile ? now_ns() : 0;
#endif
            std::optional<itch::ParsedMessage> msg;
            try {
                msg = parser.parse(*buf);
            } catch (...) {
                ++parse_errors;
                continue;
            }
#ifdef ITCH_HAVE_HDR
            if (profile) hdr.record_parse(now_ns() - t_parse_start);
#endif
            if (!msg) continue;

#ifdef ITCH_HAVE_HDR
            uint64_t t_apply_start = profile ? now_ns() : 0;
#endif
            engine.apply(*msg);
#ifdef ITCH_HAVE_HDR
            if (profile) hdr.record_apply(now_ns() - t_apply_start);
#endif
            ++messages;
        }
    } catch (const std::exception& e) {
        std::cerr << "[bench_replay] fatal: " << e.what() << "\n";
        return 1;
    }

    engine.set_pipeline_complete();

    const uint64_t t1 = now_ns();
    const double secs = double(t1 - t0) / 1e9;
    const double rate = secs > 0 ? double(messages) / secs : 0.0;

    std::cout << "[bench_replay] messages=" << messages
              << " parse_errors=" << parse_errors
              << " skipped_unknown_ref=" << engine.skipped_unknown_ref()
              << "\n";
    std::cout << "[bench_replay] wall_seconds=" << secs
              << " msg_per_sec=" << uint64_t(rate)
              << " peak_rss_kb=" << peak_rss_kb()
              << "\n";

    static const char interesting[] = {
        'R','H','A','F','E','C','X','D','U'
    };
    std::cout << "[bench_replay] type_counts:";
    for (char c : interesting) {
        std::cout << " " << c << "=" << type_counts[uint8_t(c)];
    }
    std::cout << "\n";

#ifdef ITCH_HAVE_HDR
    if (profile) hdr.dump_csvs("bench/out");
#endif

    return 0;
}
