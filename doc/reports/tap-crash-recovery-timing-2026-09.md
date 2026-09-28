# Why `tap/crash_recovery.pl` is slow, and why 4.1.0 vs 4.2.0 differed (2026-09)

## Question

The 4.2.0 TAP run took **8,187 s**; notes from the 4.1.0 release recorded
**5,379 s** on the same instance type. Was 4.2.0 (sparsemap v5.7.0) a
regression?

## Answer

No. Run time is dominated by `crash_recovery.pl`, and its cost is
`O(committed_batches x pending_list_pages)` — both driven by how much the
fixed 6-second writer commits, i.e. by disk throughput at that moment, not by
pg_tre's code. Measured back to back on one `c7i.4xlarge` (EBS gp3, PG 18.0,
`shared_buffers=128MB`):

| build | cycle-1 batches | cycle-2 batches | crash_recovery wall |
|---|---|---|---|
| 4.1.0 | 603 | 519 | **7,860 s** |
| 4.2.0 (release box) | 584 | 477 | ~8,000 s (of 8,187 s total) |

On identical hardware the two are the same. The earlier "5,379 s" was a
different run on a different instance where the writer committed fewer batches.

## Where the time goes (measured)

`crash_recovery.pl` verifies **every committed batch** with an indexed
`count(*)` after WAL replay, before any VACUUM. Every posting written by the
workload is still in the **pending list** (an unmerged overlay every query
scans in full). So per-probe latency scales with the pending list, and the
pending list grows across cycles:

| phase | pending pages | per indexed probe |
|---|---|---|
| cycle 1 verify | 13,486 (fits in 128 MB buffers) | ~3.5 s |
| cycle 2 verify | 25,111 (exceeds buffers → cold reads) | ~11 s |

The 3x jump is the working set crossing `shared_buffers`; the query plan shows
`Index Searches: 0` with `Buffers: shared hit=11401 read=25912` — a full
pending-overlay scan, mostly physical reads. 519 cycle-2 probes × 11 s ≈ 95
min, which is the bulk of the run.

## Proof it is the pending list, not the code

Same 22,387-page pending list, one throwaway cluster, probe before vs after a
`VACUUM` (which merges pending → run):

```
pending_before = 22387
probe_before   = 8793 ms
VACUUM t
probe_after    =   85 ms      # 100x faster; posting_leaf pages now serve it
probe_after2   =   82 ms
```

The postings are identical; only their location changed (pending overlay →
merged run). So the slowness is the test verifying against an ever-growing
unmerged overlay, nothing sparsemap-version-specific.

## Consequences

1. **Not a 4.2.0 regression.** crash_recovery time is a function of writer
   throughput and buffer sizing on the runner, and varies run to run.
2. **The wall time is real and worth trimming.** A `VACUUM crash_test` before
   the verify loop each cycle would collapse cycle 2's probes from ~11 s to
   ~0.1 s (≈95 min → ~1 min) while still exercising the crash-safety path
   (the kill -9 / replay / differential-check is unchanged; only the read
   after replay gets cheaper). This is a test-harness change, tracked as a
   followup — deliberately not bundled with the 4.2.0 release, which only
   needed to *measure* the cost, not alter the coverage.
3. If a firmer A/B is ever wanted, run both tags on one instance and compare
   the `commit_log` batch counts; the ratio predicts the wall-time ratio.

## Rig

`c7i.4xlarge` (16 vCPU, 30 GB), 80 GB gp3, Debian 12, gcc 12, PostgreSQL
18.0, `shared_buffers=128MB`, hotdog EC2 account, us-east-2. Both pg_tre
builds installed into separate PG prefixes on the same box from the same
tarballs, so compiler/kernel/disk/PG binaries are shared.
