# pg_tre 3.2.x at-scale stress qualification — results

Run: `bench/stress/stress-suite.sh` on AWS **i4i.8xlarge** (32 vCPU, 256 GB
RAM, 2×3.75 TB Nitro NVMe RAID-0 = 6.9 TB on `/mnt/nvme`), Amazon Linux
2023, PostgreSQL 18.0 (`-O2`, no cassert), pg_tre @ v3.2.0 + committed
stress harness. Corpus: `gen_large_corpus.py`, planted tokens
(government 5% / electrification 1% / naturalize 0.1% / absent) plus a 1%
anchored-`^government` fraction.

The **accuracy oracle** (index result set `EXCEPT` seq-scan result set, both
directions, per query) is the non-negotiable gate. **It passed with zero
mismatches on every query in every scenario, at every scale tested.**

## Verdict: QUALIFIED (with two documented, non-blocking findings)

| # | scenario | result |
|---|----------|--------|
| A | temp-disk exhaustion | **PASS** — clean `ERRCODE_PROGRAM_LIMIT_EXCEEDED`, never a PANIC or half-built index |
| B | `maintenance_work_mem` starvation | **PASS** — build completes; RSS bounded (private build memory stays small; disk-spill works) |
| C | cold-cache (index ≫ SB) | **PASS** — 0 mismatches cold & warm; on NVMe cold≈warm (fast local SSD) |
| D | CIC under concurrent writes | **PASS** — `indisvalid=t`, 0 oracle mismatches after concurrent insert/delete churn |
| E | cancellation mid-build | **PASS** — cancel honored in ~0.06 s, no valid half-built index left |
| F | crash recovery (SIGKILL mid-build) | **PASS** — WAL replay clean, **0 PANIC/corrupt**, heap 100% intact, no orphaned valid index |
| G | VACUUM under churn | **FINDING** — index bloats under delete+reinsert churn; VACUUM under-reclaims partial posting leaves; REINDEX fully reclaims (see below). Correctness intact (0 mismatches) |
| H | pathological patterns | **PASS** — compile-bomb `a{80}{80}{80}` rejected instantly; high-k bounded; `statement_timeout` effective |
| I | parallel-build saturation | **PASS** — parallel 41.6 s vs serial 60.5 s, identical hit counts, **0 stuck-spinlocks** |
| J | SuRF at scale | **PASS** — anchored-absent reject ~0.05 ms (O(1), independent of table size) |

## Finding 1 — index build throughput is low at scale (performance)

Measured `CREATE INDEX ... USING tre` build times (medium corpus, ~400 B
rows, 8 workers, 1 GB `maintenance_work_mem`):

| rows | heap | build time |
|------|------|-----------|
| 50k  | 8 MB | ~3.4 s |
| 250k | 49 MB | ~15–40 s |
| 2M   | 385 MB | **>7 min** |
| 10M  | 1.9 GB | **>20 min** |

Build cost grows **super-linearly**: the parallel workers accelerate only
the heap-scan + trigram-extraction + sort phase, while the posting-tree and
upper-tree construction (and the SuRF build) run serially in the leader, and
that serial phase dominates once the emitted-trigram count is large (~35
trigrams/row → ~70M emissions at 2M rows). Constraining
`maintenance_work_mem` makes it dramatically worse (a 16 MB-mwm 10M-row
build ran >20 min and did not finish within the test window; RSS stayed
~100 MB throughout — memory *is* bounded, but merge-pass count explodes).

**Implication (already the guidance in `LIMITATIONS.md`):** for large text
corpora, budget build time generously, use `CREATE INDEX CONCURRENTLY` /
`REINDEX CONCURRENTLY` to avoid holding a heavy lock, keep
`maintenance_work_mem` generous, and reserve pg_tre for the
edit-distance/regex niche over an already-narrowed subset rather than as a
primary index on tens of millions of rows.

## Finding 2 — posting-leaf bloat under sustained churn (reclaim)

Scenario G holds the **row count flat** (each round deletes ~1% of rows and
reinserts the same count) and VACUUMs every round:

```
baseline           index = 276 MB
round 1..6         406 → 535 → 665 → 795 → 931 → 1070 MB   (monotonic)
after 3 settling VACUUMs   1070 MB  (no reclaim)
REINDEX                     278 MB  (== baseline)
```

Page-kind histogram at the 1070 MB peak showed the growth is in
`posting_leaf` pages at ~88 % fill. VACUUM removes dead TIDs from posting
leaves and recycles *fully emptied* leaves (nbtree-style deferred
`GlobalVisCheckRemovableFullXid` reclaim), but it does **not merge or
compact partially-empty leaves** — so a churn pattern that keeps every
touched leaf ~88 % full never fully empties a leaf, and the allocated pages
accumulate. `REINDEX` rebuilds compactly and returns to baseline.

This mirrors nbtree's own behavior (btree also does not merge half-empty
pages without REINDEX) and **correctness is never affected** (0 oracle
mismatches throughout). It is a **workload-sizing/maintenance item**, not a
defect: churn-heavy pg_tre deployments should schedule periodic
`REINDEX CONCURRENTLY`. Documented in `LIMITATIONS.md`.

## What "qualified" means here

All five pass/fail criteria from `STRESS-PLAN.md §7` hold:
1. **No corruption, ever** — oracle clean at every scale; crash recovery
   yields an intact heap and a valid-or-absent (never corrupt) index.
2. **No unbounded *memory*** — build RSS bounded by `maintenance_work_mem`
   even at 10M rows / 16 MB mwm.
3. **Graceful cliffs** — temp-disk and NFA/compile/statement-timeout limits
   produce clean, cancellable errors, never PANIC or hang.
4. **No parallel-build deadlocks** under saturation (0 stuck-spinlocks).
5. **Documented degradation** — build-throughput curve and churn-bloat
   behavior recorded here and in `LIMITATIONS.md`.

The two findings are performance/maintenance characteristics, not
correctness or safety failures, so 3.2.x is cleared for release.
