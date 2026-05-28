# Phase 5: Off-thread snapshot with per-book versioning

## Problem

`OrderBookEngine::apply` calls `maybe_publish_snapshot()` every N messages
(default 1000). `build_snapshot()` walks every book and every level, copies
into vectors, allocates strings, calls `to_double()` on every price.

On the pipeline hot path that is the biggest single latency outlier:

- mean apply latency under 200 ns
- p99 apply latency a few hundred ns
- p99.9 apply latency dominated by the snapshot interval: hundreds of
  microseconds at scale (5000 instruments * dozens of levels)

A periodic spike every 1000 messages is exactly the shape of the
histogram tail an interviewer will ask about.

## Goal

Move snapshot construction off the pipeline thread. p99.9 collapses to
something close to the steady-state apply latency.

## Design

### Threads

- Pipeline thread: parses and applies. Never blocks on the snapshot.
- Snapshot thread: builds and publishes snapshots. Polls pipeline state
  via lock-free primitives.

### Per-book versioning

Each `OrderBook` gets a `uint64_t version_` and is wrapped in a seqlock.

```cpp
class BookSlot {
public:
    OrderBook book;

    // Pipeline thread: take a write window around each mutation.
    void begin_write();
    void end_write();

    // Snapshot thread: copy the book; returns false if a write was in flight
    // and the copy was inconsistent, retry.
    bool try_read_copy(OrderBook& out_copy) const;

private:
    std::atomic<uint64_t> seq_{0}; // even = stable, odd = write in flight
};

inline void BookSlot::begin_write() {
    seq_.store(seq_.load(std::memory_order_relaxed) + 1,
               std::memory_order_release);
}

inline void BookSlot::end_write() {
    seq_.store(seq_.load(std::memory_order_relaxed) + 1,
               std::memory_order_release);
}

inline bool BookSlot::try_read_copy(OrderBook& out) const {
    uint64_t s1 = seq_.load(std::memory_order_acquire);
    if (s1 & 1) return false;     // write in flight
    out = book;                   // copy under presumed stability
    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t s2 = seq_.load(std::memory_order_acquire);
    return s1 == s2;              // unchanged means our copy is consistent
}
```

Cost on the pipeline thread: two relaxed stores around every mutation.
Sub-nanosecond.

### Dirty queue

Pipeline thread maintains an SPSC ring of locate ids it touched since the
last snapshot. Snapshot thread drains it. Bounded; if it overflows,
snapshot falls back to scanning all books (rare; tune size).

```cpp
moodycamel::ReaderWriterQueue<uint16_t> dirty(8192);
```

### Snapshot thread loop

```
loop:
    wait for the wake signal from pipeline thread
    drain dirty queue -> set<locate>
    for each dirty locate:
        if try_read_copy fails, retry up to K times, else fall back to
            a write-fenced copy (single CAS-protected slow path)
        rebuild OrderBookSnapshot from the copy, replace prior snapshot
            entry in scratch under that symbol
    finalize scratch.messages_processed, scratch.snapshot_timestamp
    publisher_.publish(scratch)
    ping-pong: scratch <-> prev so next iteration reuses allocations
```

### Pipeline -> snapshot signal

Atomic counter incremented by pipeline thread; snapshot thread spins or
sleeps on a comparison. No condvar; condvars are not friendly to p99.9.

```cpp
std::atomic<uint64_t> snapshot_due_{0};
// pipeline:
if ((messages_processed_ & 1023) == 0)
    snapshot_due_.fetch_add(1, std::memory_order_release);
// snapshot:
while (snapshot_due_.load(std::memory_order_acquire) == last_seen)
    std::this_thread::yield(); // or spin with PAUSE then yield
```

### Reusing scratch ("ping-pong")

Two scratch `SystemSnapshot` slots. Snapshot thread alternates between
them. Each rebuild overwrites only the books in the dirty set; everything
else carries over. This is the 100x snapshot-cost win the plan notes.

### Edge cases that need to be correct, in order of subtlety

1. **First snapshot**: dirty set is "all". No incremental win on first
   build; subsequent builds amortize.
2. **Book first appears**: instrument was registered but no order seen;
   has to appear as an empty book in the snapshot. Handle by marking new
   locates dirty on instrument registration too.
3. **Pipeline complete (final flush)**: pipeline signals "drain
   everything, last snapshot." Snapshot thread builds a full snapshot
   (not incremental) and sets `pipeline_complete = true`.
4. **Reader sees an inconsistent copy**: handled by seqlock retry.
   Bounded retries because the pipeline only holds the write window
   around the actual mutation (nanoseconds).
5. **SPSC dirty queue overflow**: fall back to a full scan. Counter for
   how often this happens; if non-zero, increase queue size.

## What this writeup is worth

Re-running the histogrammed replay (`make bench-replay`) after this
change should produce:

- p99.9 apply latency drops by 10x or more
- mean apply latency rises slightly (the seqlock stores cost ~1 ns/op)
- snapshot publish cadence stays unchanged

That's the narrative for the README:

> A periodic snapshot every 1000 messages was running on the pipeline
> thread, causing a p99.9 tail spike of ~XX us. Moving snapshot
> construction to a dedicated thread and using a seqlock + per-book
> dirty tracking dropped p99.9 from XX us to YY us while leaving the
> mean apply path essentially unchanged (cost: 2 ns per mutation for
> the seqlock stores).

## Cut criteria

Skip Phase 5 if:

- Phase 4 alone reduces snapshot cost to under 100 us (then the tail
  story is already weak)
- the seqlock implementation does not pass an aggressive concurrent
  property test ("snapshot copies never see a level with shares < 0")
- the SPSC dirty queue overflows under capture replay

If any of those, leave the snapshot on the pipeline thread, document the
known limitation in the README, and ship Phase 6.
