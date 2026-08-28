# pg_tre at-scale adverse-conditions stress plan

Purpose: exercise pg_tre **under load, at scale, and under adverse
conditions** on a large EC2 instance with **local NVMe** — well beyond the
50k-row functional benchmark in `bench/ab-bench.sh`. This plan is committed
so future releases can re-run it verbatim and diff results.

The goal is *not* to produce marketing latencies. It is to find the
**failure modes and cliffs**: where builds run out of temp disk, where
memory bounds hold or break, where query latency degrades once the working
set exceeds RAM, whether crash recovery and VACUUM survive churn at scale,
and whether the DoS guards actually bound pathological patterns.

---

## 1. What we already know (the walls to push on)

From `LIMITATIONS.md` and prior benchmarks, the known scaling factors are:

```
build temp_disk  ≈ emitted_trigrams × ~64 B     (tuplesort spill — THE wall)
index_size       ≈ distinct_trigrams × ~16 B    (after sparsemap compression)
tid_bloom (RAM)  ≈ N_rows × ~56 B               (resident during build)
peak build RSS    bounded by maintenance_work_mem (since 1.8.0 tuplesort)
```

Natural text emits ~1 trigram per character, so a 10 GB text column implies
~640 GB of build temp disk at worst (before per-row de-dup). Local NVMe is
exactly what makes testing this affordable and fast.

Adverse conditions we deliberately induce:

| # | Scenario | Failure mode we're probing |
|---|----------|----------------------------|
| A | **Temp-disk exhaustion** during a huge build | clean `ERRCODE_PROGRAM_LIMIT_EXCEEDED` / cancellable ENOSPC vs. crash |
| B | **`maintenance_work_mem` starvation** (tiny mwm, huge corpus) | memory stays bounded (no OOM), build completes via disk spill |
| C | **Cold-cache queries** (working set ≫ shared_buffers) | latency degradation profile; NVMe read amplification |
| D | **Concurrent write load during build** (`CREATE INDEX CONCURRENTLY`) | CIC two-phase correctness under churn; no lost rows |
| E | **Cancellation mid-build** (`pg_cancel_backend` at each phase) | cancels within ~1 s; no orphaned temp/leaked pages |
| F | **Crash recovery at scale** (SIGKILL mid-build and mid-VACUUM) | WAL replay correct; index valid or cleanly gone; no corruption |
| G | **VACUUM under sustained churn** (delete/insert storm) | page reclamation keeps up; index doesn't grow unbounded |
| H | **Pathological patterns** (long literals, high k, `a{N}{N}{N}`) | `max_nfa_states` / `compile_timeout_ms` / `match_timeout_ms` bound it |
| I | **Parallel build saturation** (workers = vCPUs, many concurrent) | no deadlock/spinlock; determinism vs serial |
| J | **SuRF at scale** (100M distinct-ish trigrams) | filter size stays sane; anchored-absent reject stays O(1) |

Every data-returning query is checked against a **sequential-scan oracle**
(index result set must equal seq-scan result set) — correctness is the
non-negotiable gate at every scale.

---

## 2. Instance & storage

Local-NVMe, memory-below-dataset by design (so cold-cache/IO paths are
real, not cache hits). Defaults (override via env in `provision-ec2.sh`):

| role | instance | vCPU | RAM | local NVMe | ~$/hr (on-demand) |
|------|----------|-----:|----:|-----------|------:|
| **default** | `i4i.8xlarge`  | 32 | 256 GB | 2 × 3750 GB AWS Nitro SSD | ~$2.75 |
| big     | `i4i.16xlarge` | 64 | 512 GB | 4 × 3750 GB | ~$5.49 |
| huge    | `i3en.12xlarge`| 48 | 384 GB | 4 × 7500 GB (30 TB) | ~$5.42 |

Rationale: `i4i` Nitro SSDs sustain multi-GB/s and hundreds-of-K IOPS —
ideal for the tuplesort temp-disk wall (scenario A/B) and cold-cache reads
(C). We put **both** `$PGDATA` and the temp tablespace on the striped NVMe
so build temp and heap IO are on fast local disk, and size `shared_buffers`
*small relative to the dataset* on purpose.

The NVMe is ephemeral (lost on stop/terminate) — fine, this is a throwaway
test rig. `provision-ec2.sh` RAID-0 stripes the instance-store volumes into
one `/mnt/nvme` and puts the cluster there.

**TERMINATE when done** — `stress-suite.sh --teardown` and the provision
script both print the terminate command; a forgotten i4i.16xlarge is ~$130/day.

---

## 3. Dataset

`gen_large_corpus.py` streams a corpus of `N` rows with a configurable
mean row width, so we can independently scale row count and text length —
the two axes that drive `tid_bloom` (rows) and `temp_disk` (characters).

Three shapes, matching `LIMITATIONS.md`'s table:

- **short**: ~50 char rows (SKUs/log-lines/identifiers) — scale to 100M rows.
- **medium**: ~400 char rows (message subjects) — scale to 10–20M rows.
- **long**: ~2–4 KB rows (email/document bodies) — scale to 1–5M rows; this
  is the shape that hits the temp-disk wall first.

Planted tokens at fixed frequencies (common 5% / mid 1% / rare 0.1% /
absent) give deterministic, selectivity-controlled query points across any
scale, plus anchored variants (`^token`) for the SuRF path. The generator is
seeded (reproducible) and writes CSV to stdout or a file so it can pipe
straight into `\copy` without staging hundreds of GB twice.

`tre_estimate_index_build()` is called before each build so we record the
*predicted* vs *actual* temp-disk and index size — validating the sizing
model itself is one of the deliverables.

---

## 4. Scenarios (what `stress-suite.sh` runs)

Each scenario writes a raw CSV row to `results/<host>-<ts>/<scenario>.csv`
and a human summary to `SUMMARY.md`. Scenarios are independently
selectable (`--only A,C,F`) so a partial re-run is cheap.

### A — Temp-disk exhaustion (build cliff)
Build the largest **long** corpus that fits, then deliberately cap temp
space (small temp tablespace on a size-limited mount, or
`pg_tre.build_max_entries_mb`) and confirm the build fails with a *clean,
cancellable* error — never a PANIC or a half-written index. Record the
emitted-trigram count at failure vs the `tre_estimate_index_build`
prediction.

### B — Memory starvation
`maintenance_work_mem = 16MB` against a 10M-row medium corpus. Sample peak
RSS of the leader (and workers) every second via `/proc/<pid>/status`.
Assert peak stays within a small multiple of `maintenance_work_mem`
(bounded, no growth with corpus) and the build completes.

### C — Cold-cache query degradation
`shared_buffers = 4GB` against a >64 GB index. Drop caches
(`pg_prewarm` off, `echo 3 > drop_caches`, restart), then run the query
matrix cold and warm; report the cold/warm ratio and NVMe read throughput
(`iostat`). This is where "index bigger than RAM" behavior lives.

### D — CIC under concurrent writes
Start a background writer (`pgbench` custom script inserting/updating/deleting
the indexed table at a steady rate), then `CREATE INDEX CONCURRENTLY`. After
it completes, run the accuracy oracle: the index must equal a fresh seq scan
of the *post-build* table state for every planted token. Repeat with
`REINDEX INDEX CONCURRENTLY`.

### E — Cancellation matrix
For each build phase (heap scan, sort, posting build, upper bulkload, SuRF
write), start a build in one session and `pg_cancel_backend` after a delay
that lands in that phase; assert cancel honored < 2 s and no leaked
temp files / the index is absent (not half-built-and-valid).

### F — Crash recovery at scale
`kill -9` the postmaster (a) mid-build, (b) mid-VACUUM, (c) mid-CIC. Restart,
let WAL replay, then: (1) recovery completes with no PANIC, (2) any index
present is `indisvalid` AND passes the oracle, (3) `tre_surf_stats` /
`pg_tre_index_format_status` read cleanly. Uses the custom rmgr replay path
that the TAP tests exercise — but at 10M+ rows.

### G — VACUUM under churn
Steady delete+insert storm (10% of rows/min churn) for N minutes with
autovacuum on; sample index size + `pages_deleted`/`tuples_removed` from
`pg_stat`/our own NOTICEs. Assert the index reaches a steady state (doesn't
grow unbounded) and stays correct.

### H — DoS / pathological patterns
Fire `a{80}{80}{80}`, 4 KB literal patterns, high-k (`k=5`) on long rows,
deeply nested alternations. Assert each is bounded by the relevant guard
(`max_nfa_states`, `compile_timeout_ms`, `match_timeout_ms`,
`max_extraction_fanout`) and raises a clean cancel/error, not a hang. Verify
`statement_timeout` aborts a broad fuzzy scan promptly (the 3.0.1 fix).

### I — Parallel-build saturation
Build with `max_parallel_maintenance_workers` = vCPUs, and run 4–8 large
builds concurrently (contending for workers, temp disk, buffers). Assert no
stuck-spinlock/deadlock (the 3.1.0 regression class), builds complete, and
each parallel-built index == its serial-built twin (differential).

### J — SuRF at scale
On the 100M-row short corpus, record SuRF key/node/page count and image
size (should stay KB–MB, sublinear in rows since it's over *distinct*
trigrams), and confirm anchored-absent reject latency stays flat (~O(1),
sub-millisecond) regardless of table size — the whole point of the tier.

---

## 5. Methodology

- **A/B alternation** where comparing variants (serial vs parallel, cold vs
  warm, 3.2.0 vs prior); ≥5 reps; report p50/p95 and min (noise-robust).
- **Accuracy oracle on every data query** — index result set `EXCEPT` seq
  result set must be empty, both directions.
- **Fair TRE**: the vendored TRE progress-hook patch MUST be applied on both
  sides of any A/B (a prior benchmark was invalidated by comparing patched
  vs pristine TRE — see `bench/stress/README.md` gotchas).
- **Raw CSV kept**; medians computed in `report`. Note kernel, instance
  type, NVMe layout, PG version, pg_tre git SHA in every result dir.
- **Cost discipline**: the suite estimates wall-clock up front and the
  provision script tags the instance with an auto-terminate hint; always
  run `--teardown`.

---

## 6. Deliverables (committed under `bench/stress/`)

- `STRESS-PLAN.md` — this document.
- `provision-ec2.sh` — launch NVMe instance, RAID-0 the instance store,
  tune the OS (THP off, governor performance), build PG + pg_tre, print the
  terminate command.
- `gen_large_corpus.py` — seeded, streaming, shape/scale-parameterized corpus.
- `stress-suite.sh` — the scenario runner (`--only`, `--scale`, `--teardown`).
- `README.md` — how to run end-to-end, interpret results, and tear down.
- `results/` — result dirs are gitignored except a committed `SUMMARY` per
  release (like `RESULTS-v2.0-ab.md`).

## 7. Pass/fail criteria (the point of the exercise)

A release "passes" the stress suite when:
1. **No corruption, ever** — oracle clean at every scale; crash recovery
   always yields a valid-or-absent index.
2. **No unbounded resource use** — build RSS bounded by `maintenance_work_mem`;
   VACUUM reaches steady state under churn.
3. **Graceful cliffs** — temp-disk / NFA / timeout limits produce clean,
   cancellable errors, never PANIC or hang.
4. **No parallel-build deadlocks** under saturation.
5. **Documented degradation** — cold-cache and high-selectivity latency
   curves recorded so users can size deployments (feeds `LIMITATIONS.md`).
