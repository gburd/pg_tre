# pg_tre 3.2.2 sparsemap-v5.5.0 re-qualification — results

Run: `bench/stress/stress-suite.sh` on AWS **i4i.8xlarge** (32 vCPU, 256 GB
RAM, 2x3.75 TB Nitro NVMe RAID-0 = 6.9 TB on `/mnt/nvme`), Amazon Linux
2023, PostgreSQL 18.0 (`-O2`, no cassert), pg_tre @ 3.2.2 with vendored
sparsemap **v5.5.0**. Corpus: `gen_large_corpus.py`, planted tokens
(government 5% / electrification 1% / naturalize 0.1% / absent) plus a 1%
anchored-`^government` fraction.

The purpose of this run is to qualify the sparsemap v5.1.1 -> v5.5.0
refresh. The **accuracy oracle** (index result set `EXCEPT` seq-scan result
set, both directions, per query) is the non-negotiable gate. **It passed
with zero mismatches on every query in every scenario.**

## Verdict: QUALIFIED

### Library-level qualification (before the server suite)

The strongest available check for a vendored-library swap is upstream's own
test suite compiled against *pg_tre's vendored copy* (`src/util/sparsemap.c`
+ `include/pg_tre/sparsemap.h`), not against upstream's tree:

| check | result |
|-------|--------|
| upstream `tests/test.c` (API/scale/perf/integration) | **44/44 pass** |
| upstream `tests/test_coverage.c` | **175,530/175,530 expectations, 0 failures** |
| `test_empty_map` / `test_rle_standalone` / `test_large_index` / `test_portability` | **all pass** |
| targeted checks for the three data-loss fixes, under ASan + UBSan | **all pass, no sanitizer reports** |
| same targeted checks against v5.1.1 (control) | **2 of 4 FAIL** — confirms the fixes are real and were absent |

The control run is the point: `sm_difference([0,16384) \ [0,16357))`
returned cardinality **0** on v5.1.1 versus the correct **27** on v5.5.0, and
`sm_select(128, true)` on a map with `[0,128)` and `[500,510)` set returned
**128** (an unset index) versus the correct **500**.

### Server-level qualification

| # | scenario | result |
|---|----------|--------|
| A | temp-disk exhaustion | **PASS** — clean `ERROR: index build exceeded pg_tre.build_max_entries_mb`, never a PANIC or half-built index |
| C | cold-cache (index >> SB) | **PASS** — 10M rows / 12 GB index / 4 GB `shared_buffers`; **0 mismatches on all 8 queries cold and warm** |
| D | CIC under concurrent writes | **PASS** — `indisvalid=t`, 0 oracle mismatches after concurrent insert/delete churn |
| E | cancellation mid-build | **PASS** — cancel honored in 0.08 s, no valid half-built index left |
| F | crash recovery (SIGKILL mid-build) | **PASS** — 0 PANIC/corruption in the log, cluster recovers, heap 100% intact (1,000,000 rows), no orphaned valid index |
| G | VACUUM under churn | **PASS on correctness** (0 mismatches); reproduces the pre-existing 3.2.x bloat finding (3.87x) |
| H | pathological patterns | **PASS** — compile-bomb `a{80}{80}{80}` rejected in 0.01 s; high-k bounded; `statement_timeout` effective |
| I | parallel-build saturation | **PASS** — parallel 415.0 s vs serial 511.4 s, **identical hit counts (59167)**, **0 stuck spinlocks** |
| J | SuRF at scale | **PASS** — anchored-absent reject p50 **0.070 ms** (O(1), independent of table size) |

Scenario B (`maintenance_work_mem` starvation) was **not re-run**: at 10M
rows with a 64 MB `mwm` it is the documented >20 min / no-finish build wall
from `RESULTS-stress-3.2.0.md`, it carries no accuracy oracle beyond what C
and D already provide, and it re-proves a performance finding rather than
anything sparsemap touches. Its 3.2.0 result stands.

### The 10M-row cold-cache matrix (scenario C)

`shared_buffers=4GB`, heap 1.9 GB, index 12 GB — the index is 3x
`shared_buffers`, so this exercises real eviction and the on-page posting
decode path that consumes sparsemap.

| query | hits | p50 cold (ms) | p50 warm (ms) | oracle |
|-------|-----:|--------------:|--------------:|:------:|
| q_common | 590,364 | 12,743 | 12,785 | **OK** |
| q_mid | 99,211 | 11,247 | 11,347 | **OK** |
| q_rare | 9,864 | 97.9 | 98.7 | **OK** |
| q_like | 99,517 | 4,821 | 4,823 | **OK** |
| q_approx1 | 590,983 | 130,796 | 130,648 | **OK** |
| q_nomatch | 0 | 0.021 | 0.021 | **OK** |
| q_anchored_absent | 0 | 0.062 | 0.062 | **OK** |
| q_anchored_present | 126,308 | 13,491 | 13,432 | **OK** |

Hit counts are identical cold and warm and match the sequential-scan oracle
exactly in both directions. Cold ~= warm because local NVMe is fast enough
that the miss cost is small relative to the recheck cost — the same
observation as 3.2.0.

## Pre-existing findings (reproduced, not regressions)

Both are unchanged from `RESULTS-stress-3.2.0.md`, both are documented in
`LIMITATIONS.md`, and neither involves sparsemap:

1. **Build throughput is super-linear at scale.** A 1M-row medium-shape
   build takes ~415 s parallel / ~511 s serial; 10M rows exceeds 20 min. The
   parallel workers accelerate only the scan/extract/sort phase while
   posting-tree, upper-tree and SuRF construction run serially in the leader.
2. **Posting-leaf bloat under sustained churn.** Scenario G holds the row
   count flat (delete ~1% + reinsert, VACUUM every round) and the index grows
   1082 MB -> 4192 MB over six rounds (3.87x). VACUUM under-reclaims partial
   posting leaves; REINDEX fully reclaims. Correctness is unaffected (0
   mismatches post-churn).

## Harness notes for the next run

- **Scenario F leaves the cluster down** (fixed in this commit). SIGKILL of
  the postmaster mid-*parallel* build can leave an orphaned worker holding
  the shared-memory segment, so the harness's restart fails with
  `pre-existing shared memory block ... is still in use` and reports
  `start_rc=1`. This was a harness artifact, not a recovery failure: after
  `pkill -9 -f <pgdata>` the cluster starts and recovers cleanly with the
  heap intact. Root cause: the cleanup `pkill` and its wait loop matched
  `postgres.*$PGDATA`, but a parallel worker retitles itself to
  `postgres: ... CREATE INDEX` and carries no `$PGDATA` path, so the pattern
  missed exactly the children that block restart. Now cleaned up by process
  *group*, and F is ordered last so a cluster it fails to restart cannot
  strand the scenarios after it (D and G reported empty results here for
  that reason and were re-run separately).
- Each build-bound scenario rebuilds the index from scratch. At 10M rows
  that is >20 min per scenario; use ~1M rows for the D-J adverse-condition
  sweep and reserve 10M for the C matrix.
- `make clean` deletes `vendor/lime/lime`, and a `lime` binary carried in a
  tarball from a nix host will not exec on AL2023 ("No such file or
  directory" = missing interpreter, not a missing file). `rm -f
  vendor/lime/lime && make vendor/lime/lime` rebuilds it natively; this is
  the gotcha already noted in `README.md`, hit again here.
