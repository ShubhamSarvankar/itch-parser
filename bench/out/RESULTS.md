# Bench Results Summary

| group | bench | key metric | notes |
|-------|-------|------------|-------|
| Group 1: Parser micros | BM_ParseAddOrder | 18.2 ns mean | In range (HDR parse p50=38 ns includes dispatch+apply setup); BM measures parse only |
| Group 1: Parser micros | BM_ParseOrderDelete | 4.4 ns mean | Fast: fixed-offset reads, minimal branching |
| Group 1: Parser micros | BM_ParseOrderReplace | 4.7 ns mean | Similar to Delete; slightly more fields |
| Group 1: Parser micros | BM_ParseUnknownType | 1.4 ns mean | Near-zero: single switch miss, no work done |
| Group 2: Book ops | BM_DeleteOrder/10k | 16.0 ns mean | L2-hot; 10k orders = ~160 KB fits in L2. Hash lookup + erase + level update. Handoff expected 150-300 ns for cold working set. |
| Group 2: Book ops | BM_ReplaceOrder/10k | 5.1 ns mean | L2-hot; cycles same n orders without draining — extra warm. |
| Group 2: Book ops | BM_ExecuteOrder/10k | 3.5 ns mean | L2-hot; minimal work (shares decrement + level update). |
| Group 3: AddOrder | BM_AddOrder_HotBand | 119 ns | Adding to an existing price level (++total_shares only); ratio vs ColdPrice = 2.1× |
| Group 3: AddOrder | BM_AddOrder_ColdPrice | 253 ns | New price level = std::map insert + node alloc; 2.1× slower than HotBand |
| Group 4: Mixed synth | BM_ApplyMixed_Synth/10k | 37.6 ns/op | 75/20/5 delete/add/execute mix; 10k book fits in L2, very hot |
| Group 4: Mixed synth | BM_ApplyMixed_Synth/100k | 71.2 ns/op | 100k book spills to L3; 1.9× slower — shows cache pressure scaling |
| Group 5: Capture replay | BM_ApplyMixed_Capture/100k | 62.5 ns/op | Real ITCH msg distribution; 100k window replayed in tight loop, fits in L3; lower than HDR mean (351 ns) due to no IO cost |
| Group 6: Snapshot build | BM_BuildSnapshot/100/20 | 21.3 us | Small book (100 instr × 20 levels); baseline snapshot cost |
| Group 6: Snapshot build | BM_BuildSnapshot/1000/50 | 575 us | Mid-scale; 27× slower than 100-instr — scales super-linearly with level count |
| Group 6: Snapshot build | BM_BuildSnapshot/5000/100 | 11.6 ms | NASDAQ session scale; at snap=1000 this fires every ~86 ms — dominates wall clock by 30× |
| Group 7: Ladder shootout | BM_Ladder_Map_Bid/10k | 17.6 ns mean, CV=1.4% | RB-tree insert; pointer-chasing baseline |
| Group 7: Ladder shootout | BM_Ladder_Flat_Bid/10k | 13.6 ns mean, CV=1.9% | Sorted vector; 1.3× faster than Map; cache-friendly at low level counts |
| Group 7: Ladder shootout | BM_Ladder_Array_Bid/10k | 11.9 ns mean, CV=0.7% | Dense array; fastest and most stable; 1.5× faster than Map |
| Group 7: Ladder shootout | BM_Ladder_Map_Top/10k | 0.21 ns mean | begin() on std::map is O(1) cached; sub-ns |
| Group 7: Ladder shootout | BM_Ladder_Flat_Top/10k | 0.30 ns mean | front() on sorted vector; also sub-ns |
| Group 7: Ladder shootout | BM_Ladder_Array_Top/10k | 0.28 ns mean | Cached best-slot pointer; sub-ns, on par with Flat |

---

## Final state summary

All 7 microbench groups completed. JSON + log pairs in bench/out/ for each.
README §1, §2, §3, §4 fully populated; zero __ placeholders remain.
Test suite: 99/99 pass (no regressions from any changes this session).
Item 3 (order_index_ reserve) intentionally skipped — the 58 ms max is documented
as a known limitation in §4 with the one-line fix called out; the RSS trade-off
(181 MB → ~3 GB) makes it a deployment decision, not an automatic win.

Deviations from original plan:
- itch_hardening_plan__1_.md referenced in handoff doc was not present on disk (non-blocking).
- §2 shootout table restructured: separate add p50/p99/erase p50 columns replaced with
  mixed-workload median + footnotes, since the bench measures a mixed workload not
  isolated ops. Honest representation of what was actually measured.
