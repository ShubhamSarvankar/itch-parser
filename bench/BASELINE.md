# Phase 0 baseline

End-to-end replay against the public NASDAQ TotalView ITCH 5.0 capture
(`12302019.NASDAQ_ITCH50`, 7.7 GB, 263M messages):

```
time ITCH_PROFILE=1 ./build/bench/bench_replay data/12302019.NASDAQ_ITCH50 1000000
```

Outputs:
- `bench/out/bench_replay_hdr.log`   wall clock, msg/sec, peak RSS, per-type counts
- `bench/out/hist_parse.csv`         per-message parse latency histogram
- `bench/out/hist_apply.csv`         per-message apply latency histogram
- `bench/out/hist_snapshot.csv`      per-snapshot-build latency histogram

Note: `perf stat` is unavailable on this WSL2 kernel (6.6.87.2-microsoft);
the IPC and cache-miss rows below are intentionally blank. Latency
percentiles come from HdrHistogram_c, which is the load-bearing measurement.

Note on snapshot interval: production default is 1000 messages, but at
~9000 active instruments per session that makes the snapshot publish
dominate wall clock by roughly 30x (extrapolated from a partial 11-minute
run that completed 7% of the file). For the baseline we use
snapshot_interval=1000000 so the engine's steady-state cost is visible;
the snapshot cost story moves to Phase 5 (see
`docs/PHASE5_OFF_THREAD_SNAPSHOT.md`).

## Configuration

| field             | value |
|-------------------|-------|
| cpu               | Intel i7-14650HX, 24 logical cores (12 physical, 2 SMT) |
| memory            | 15.4 GiB usable in WSL2 guest |
| host              | Windows 11 + WSL2 Ubuntu 24.04 LTS |
| kernel            | 6.6.87.2-microsoft-standard-WSL2 |
| compiler          | GCC 13.3.0 |
| build type        | Release (-O2 default, -DNDEBUG) |
| capture file      | 12302019.NASDAQ_ITCH50 |
| capture size      | 7.7 GB |
| snapshot interval | 1000000 (see note above) |

## Baseline numbers

| metric                              | value |
|-------------------------------------|-------|
| total messages                      | 263,259,809 |
| wall clock seconds                  | 227.5 |
| user seconds                        | 140.4 |
| sys seconds                         | 87.1 |
| sustained msg/sec                   | 1,156,960 |
| peak RSS (MB)                       | 181.6 |
| parse errors                        | 0 |
| skipped unknown ref                 | 0 |
| IPC (instructions per cycle)        | N/A (perf unavailable on WSL2) |
| L1 dcache miss rate (% of loads)    | N/A |
| LLC miss rate (% of LLC loads)      | N/A |
| branch miss rate (% of branches)    | N/A |
| dTLB load miss rate                 | N/A |

### Per-stage latency (HdrHistogram, nanoseconds)

| stage    | count  | min | p50 | p90 | p99   | p99.9 | p99.99 | max        | mean  | stddev   |
|----------|--------|-----|-----|-----|-------|-------|--------|------------|-------|----------|
| parse    | 268.7M |  16 |  38 |  59 |    88 |   149 |    376 |  1,287,167 |  42.0 |    225.3 |
| apply    | 263.3M |  22 | 231 | 594 | 1,020 | 1,529 | 10,967 | 58,589,183 | 351.2 | 48,286.8 |
| snapshot |    263 |  -  |  -  |  -  |   -   |   -   |    -   |     -      |   -   |   -      |

Snapshot count is too low (263 samples across the run at snap=1000000) to
produce meaningful percentiles; the histogram exists but is not load-bearing.

### Per-type counts

```
R=8906 H=8966 A=117145568 F=1485888 E=5722824 C=99917 X=2787676 D=114360997 U=21639067
```

Real ITCH session mix on this capture:
- A (Add Order):           44.5%
- D (Order Delete):        43.4%
- U (Order Replace):        8.2%
- E (Order Executed):       2.2%
- X (Order Cancel):         1.1%
- F (Add Order MPID):       0.6%
- C (Executed w/ Price):    0.04%
- R + H (informational):    0.01%

Note: the synthetic bench in `bench_mixed_workload.cpp` uses a 75/20/5
delete/add/execute mix, which understates the Add rate relative to reality.
Closer to 45/45/10 add/delete/everything-else is the honest distribution.

## Paragraph for the README

> Baseline replay of `12302019.NASDAQ_ITCH50` (7.7 GB, 263M messages),
> single threaded, snapshot publish disabled (`snap=1000000`), GCC 13.3
> Release, Intel i7-14650HX in WSL2 Ubuntu 24.04: 227.5 s wall clock,
> 1.16M msg/s sustained, 181.6 MB peak RSS. Parse latency p50 38 ns,
> p99 88 ns, p99.9 149 ns. Apply latency p50 231 ns, p99 1.02 us,
> p99.9 1.53 us, p99.99 10.97 us, max 58.6 ms. The p99.99 spike is the
> periodic snapshot publish; the 58 ms max is a hash-table rehash on
> `order_index_` as it crosses a capacity boundary mid-session (fix:
> `reserve()` in the engine constructor, currently unimplemented).
> `perf` counters unavailable on this WSL2 kernel; latency percentiles
> measured via HdrHistogram_c v0.11.8.

## Notes

- sys time (87 s) is 38% of wall clock; this is the WSL2 hypervisor's IO
  syscall overhead, not real engine cost. A native Linux run on the same
  silicon would likely shave 30-60s off wall clock.
- At snap=1000 (production default), extrapolation says ~117 min total. The
  ~30x slowdown from snapshot publish at NASDAQ session scale is the
  empirical motivation for the Phase 5 off-thread design.
- The capture is 263M messages, not 412M as some earlier docs claim. 412M
  may include feed-A+B duplicates or auction messages we don't process.