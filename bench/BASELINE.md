# Phase 0 baseline

Run the replay end to end against the public NASDAQ TotalView ITCH 5.0 capture
(`12302019.NASDAQ_ITCH50`, ~7.7 GB, ~412M messages):

```
make perf-baseline ITCH_FILE=data/12302019.NASDAQ_ITCH50
```

Outputs:
- `bench/out/baseline.log`   wall clock, msg/sec, peak RSS, per-type counts
- `bench/out/baseline.perf`  IPC, branch miss rate, L1/LLC miss rate, dTLB misses

Copy the numbers into the table below. Do not edit anything else in the repo
between this run and any "after" run; otherwise the comparison is not honest.

## Configuration

| field             | value |
|-------------------|-------|
| cpu               |       |
| memory            |       |
| compiler          |       |
| build type        | Release (-O2 -march=native -DNDEBUG) |
| capture file      | 12302019.NASDAQ_ITCH50 |
| capture size      |       |
| snapshot interval | 1000  |

## Baseline numbers

| metric                              | value |
|-------------------------------------|-------|
| total messages                      |       |
| wall clock seconds                  |       |
| sustained msg/sec                   |       |
| peak RSS (MB)                       |       |
| parse errors                        |       |
| skipped unknown ref                 |       |
| IPC (instructions per cycle)        |       |
| L1 dcache miss rate (% of loads)    |       |
| LLC miss rate (% of LLC loads)      |       |
| branch miss rate (% of branches)    |       |
| dTLB load miss rate                 |       |

Per-type counts: ` ` (paste the `type_counts` line).

## Paragraph for the README

Replace the placeholders with the actual numbers, then drop this into the
README under "Headline performance".

> Baseline replay of `12302019.NASDAQ_ITCH50` (X.X GB, NNN M messages):
> wall clock SS.S s, X.X M msg/s sustained on <cpu>. IPC X.XX, L1 dcache miss
> rate X.X%, LLC miss rate X.X%, branch miss rate X.X%. Snapshot publish runs
> every 1000 messages and is included in the cost. Numbers are reproducible by
> running `make perf-baseline` against the same capture file.

No tuning, no defending, no "we will improve this." That is the point. The
baseline is the number to beat in later phases.
