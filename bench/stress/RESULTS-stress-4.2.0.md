# Stress results: pg_tre 4.2.0

This release fixes a backend abort in the similarity family and vendors
sparsemap **v5.6.0 → v5.7.0**.

## Rig

| | |
|---|---|
| Instance | `c7i.4xlarge` (16 vCPU, 30 GB), 80 GB gp3, us-east-2 |
| OS / compiler | Debian 12.15, gcc 12.2.0 |
| PostgreSQL | 18.0, `--enable-tap-tests`, `data_checksums=on` |
| pg_tre | 4.2.0 (TRE 0.9.0), sparsemap 5.7.0 |
| Corpus | 1,000,000 rows, shape=medium |

## Gates

| Gate | Result |
|---|---|
| Build | 0 warnings, 0 errors |
| Regression | **47/47** |
| TAP | **17/17**: concurrency 2/2, crash_recovery 10/10, replication 5/5 (8,187 s) |
| Accuracy oracle | **0 mismatches** (16 panels) |
| Stuck spinlocks | 0 |

## Stress scenarios

| Scenario | 4.2.0 | 4.1.0 |
|---|---|---|
| C: cold-cache | oracle clean | oracle clean |
| D: CIC under writes | `valid=t`, 0 mismatches, 59,167 hits | same |
| G: VACUUM under churn | **PASS, 1.98x** (2146 MB) | PASS, 1.98x (2146 MB) |
| H: DoS guards | compile_bomb 0.01 s; big_k 7.73 s | 0.01 s; 8.05 s |
| I: parallel build | 472.4 s / serial 571.5 s, same_hits=YES | 485.3 s / 628.3 s |
| J: SuRF reject p50 | 0.064 ms | 0.070 ms |

G plateaus at the same size as 4.1.0, so the new small-set encoding did not
change the churn footprint at this scale.

## Similarity crash (the reported bug)

| Build | `tre_trgm_similarity('foo','foobar')` |
|---|---|
| unfixed, ASan server | backend lost: `stack-buffer-overflow` in `trgm_set:94`, `'wc'` |
| unfixed, ASan server, `tre_word_similarity` | backend lost: `pos_trgm:343`, `'wc'` |
| fixed, ASan server | `0.375`, ASan silent across `similarity_multibyte.sql` |
| unfixed, stock `-O2` ± `-fstack-protector-all` | `0.375`: **does not reproduce here** |

The last row explains how the bug went unnoticed. The write is out of bounds
on every build, but it only crashes where the frame layout puts the canary
next to `wc`. See `doc/reports/similarity-stack-overwrite-2026-09.md`.

## sparsemap v5.7.0

| Check | Result |
|---|---|
| 17 consumed `sm_*` signatures | unchanged |
| Forward read (5.6.0 writes → 5.7.0 reads) | **10/10**, no rejections |
| 4.1.0 guard vs new small-set encoding | **0 false positives** / 10 shapes (4 small-mode) |
| Downgrade read (5.7.0 writes → 5.6.0 reads) | 4 rejected, 6 fine, **0 silent misreads** |
| 4.1.0 guard on those 4 rejections | fires on **all 4** (loud REINDEX error) |
| Upstream suite vs vendored copy | 17/17 suites, incl. the 5 new ones; 175,543 coverage expectations |
| Strict-flag warnings | 0 (`-Wconversion -Wsign-conversion -Wshadow -Wpedantic`) |
| `SPARSEMAP_PREFIX` | 85 symbols, 0 leaks |
| Pending-list probe, 5.6.0 vs 5.7.0 | 415 ms vs 423 ms (+2%) |

## Notes

- **TAP timing.** The first TAP attempt was cut off by a 7,800 s harness
  timeout while inside `crash_recovery.pl`, and could easily be misread as a
  hang. Timing individual probes showed otherwise: each of the ~1,060
  committed batches is checked with an indexed probe over a pending list of
  ~13,000 pages, at 3.5-4.9 s per probe. The full run needs about 2h15m on
  this EBS rig, and a 9,000 s budget passes.
- **Unexplained difference.** The same TAP suite took 5,379 s for 4.1.0 on
  the same instance type, versus 8,187 s here. I have not isolated why. The
  sparsemap swap on its own measured +2% per pending-list probe (415 → 423
  ms, same pg_tre source), which is far too small to account for +52%. The
  test's cost grows superlinearly with how many batches the writer commits
  in its fixed 6 s window: more batches means both more probes and a longer
  pending list per probe. Writer throughput on a fresh gp3 volume is a
  plausible cause, but 4.1.0's batch count wasn't recorded, so this is
  unconfirmed. If it matters, re-run 4.1.0 and 4.2.0 on one instance and
  compare `commit_log` counts.
- This box has no instance NVMe, so `/mnt/nvme` is gp3. Absolute latencies
  are not comparable to NVMe runs; the A/B and determinism gates are
  unaffected.
