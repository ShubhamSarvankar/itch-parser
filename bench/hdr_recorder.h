// bench/hdr_recorder.h
//
// Thin wrapper around HdrHistogram_c. Always-on when ITCH_HAVE_HDR is
// defined and enable() was called. Records nanosecond latencies for parse,
// apply, and snapshot build, dumps three CSVs at shutdown.
//
// Range: 1 ns to 60 s, 3 significant digits. Hot path is one branch and one
// hdr_record_value call per sample, ~10-20 ns.

#pragma once

#ifdef ITCH_HAVE_HDR

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <hdr/hdr_histogram.h>
#include <string>
#include <sys/stat.h>

namespace itch::bench {

class HdrRecorder {
public:
    HdrRecorder() {
        hdr_init(1, 60'000'000'000LL, 3, &h_parse_);
        hdr_init(1, 60'000'000'000LL, 3, &h_apply_);
        hdr_init(1, 60'000'000'000LL, 3, &h_snapshot_);
    }
    ~HdrRecorder() {
        if (h_parse_)    hdr_close(h_parse_);
        if (h_apply_)    hdr_close(h_apply_);
        if (h_snapshot_) hdr_close(h_snapshot_);
    }

    void enable()  { enabled_ = true; }
    bool enabled() const { return enabled_; }

    inline void record_parse(int64_t ns) {
        if (enabled_ && ns > 0) hdr_record_value(h_parse_, ns);
    }
    inline void record_apply(int64_t ns) {
        if (enabled_ && ns > 0) hdr_record_value(h_apply_, ns);
    }
    inline void record_snapshot(int64_t ns) {
        if (enabled_ && ns > 0) hdr_record_value(h_snapshot_, ns);
    }

    void dump_csvs(const std::string& dir) const {
        if (!enabled_) return;
        ::mkdir(dir.c_str(), 0755);
        dump_one(h_parse_,    dir + "/hist_parse.csv",    "parse");
        dump_one(h_apply_,    dir + "/hist_apply.csv",    "apply");
        dump_one(h_snapshot_, dir + "/hist_snapshot.csv", "snapshot");
    }

private:
    static void dump_one(const hdr_histogram* h, const std::string& path,
                         const char* label) {
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "stage,count,min_ns,p50_ns,p90_ns,p99_ns,p999_ns,"
                        "p9999_ns,max_ns,mean_ns,stddev_ns\n");
        std::fprintf(f, "%s,%" PRIu64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ","
                        "%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64
                        ",%.1f,%.1f\n",
            label,
            h->total_count,
            hdr_min(h),
            hdr_value_at_percentile(h, 50.0),
            hdr_value_at_percentile(h, 90.0),
            hdr_value_at_percentile(h, 99.0),
            hdr_value_at_percentile(h, 99.9),
            hdr_value_at_percentile(h, 99.99),
            hdr_max(h),
            hdr_mean(h),
            hdr_stddev(h));
        std::fclose(f);
    }

    hdr_histogram* h_parse_{nullptr};
    hdr_histogram* h_apply_{nullptr};
    hdr_histogram* h_snapshot_{nullptr};
    bool enabled_{false};
};

} // namespace itch::bench

#endif // ITCH_HAVE_HDR
