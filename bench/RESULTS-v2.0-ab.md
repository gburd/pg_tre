# pg_tre A/B benchmark results — v2.0 (defaults-on) vs v1.12.0 vs pg_trgm

Two hosts, both PostgreSQL 18.4, self-contained ephemeral clusters
(`bench/ab-bench.sh`). Corpus: 50,000 rows, ~150 B each, realistic
selectivity (planted tokens at 5% / 1% / 0.08%), `bench/gen_corpus_realistic.py`.

- **nuc** — FreeBSD 15 / amd64, 8-core, dedicated/quiet → **low-variance,
  authoritative timings** (p50 ≈ p95). PG18 + the vendored TRE built from
  source (TRE autotools bootstrap done off-host via the nix flake's toolchain,
  artifacts overlaid — see ab-bench `--tre-bootstrap`).
- **meh** — Linux 6.12 / x86_64, 24-core, **shared/noisy** → useful for
  accuracy + ratios, but absolute latencies are inflated ~7× and high-variance.
  Treat nuc as the timing source of truth.

## Accuracy (the oracle) — BOTH hosts, BOTH versions

**Every query: index result set == sequential-scan result set (0 mismatches),**
on FreeBSD/amd64 and Linux/x86_64, for pg_tre v2.0, pg_tre v1.12.0, and pg_trgm.
pg_tre is exact (recheck authoritative). This is the headline correctness result
and a genuine cross-platform confirmation.

## Build + size (platform-independent)

| index | build (nuc) | size | vs pg_trgm |
|-------|------------:|-----:|-----------:|
| pg_tre v2.0 | 7.06 s | 296.4 MB | 24.3× larger |
| pg_tre v1.12.0 | 7.72 s | 297.2 MB | 24.4× larger |
| pg_trgm (GIN) | 0.90 s | 12.2 MB | — |

Coalescing (when enabled) gave **no size win** (296 vs 297 MB): on this corpus
the medium bucket (serialized sparsemap 2048–3072 B) is nearly empty. The 24×
size gap vs pg_trgm is the real story and is unaffected by coalescing.

## Latency — pg_tre v2.0 vs pg_trgm (nuc, p50 ms)

| query | selectivity | hits | pg_tre v2.0 | pg_trgm | ratio |
|-------|------------:|-----:|------------:|--------:|------:|
| rare `naturalize` (`~`) | 0.08% |   38 |   0.46 | 0.28 | trgm 1.6× |
| mid `electrification`   | 1.0%  |  486 |  96.5  | 1.97 | trgm 49× |
| common `government`     | 5.0%  | 2474 | 204.4  | 5.39 | trgm 38× |
| LIKE `%electrific%`     | 1.0%  |  488 |  59.4  | 0.90 | trgm 66× |
| nonselective `the`      | 45%   |22513 | 261.3  | 22.1 | trgm 12× |
| approx1 `govrnment` k=1 | —     | 2477 | 719.4  | (n/a) | pg_tre only |

pg_trgm is 1.6–66× faster on shared exact/LIKE/regex-literal queries — expected
and by design (pg_tre rechecks every candidate with the full TRE regex engine).
pg_tre's value is **approximate edit-distance regex** (q5: k=1 fuzzy), which
pg_trgm cannot express. North star intact: pg_tre is a regex index *with edit
distance*, not a faster pg_trgm.

## Self comparison — v2.0 (defaults-on) vs v1.12.0 (nuc, p50 ms)

| query | v2.0 | v1.12.0 | delta |
|-------|-----:|--------:|------:|
| common `government`   | 204.4 | 203.4 | +0.5% |
| mid `electrification` |  96.5 |  96.4 | ~0% |
| rare `naturalize`     |  0.46 |  0.44 | +4% (sub-ms) |
| LIKE `%electrific%`   |  59.4 |  59.7 | −0.5% |
| approx1 `govrnment`   | 719.4 | 692.8 | +3.8% |
| nonselective `the`    | 261.3 | 265.5 | −1.6% |

**Essentially neutral** — all within measurement noise on the clean machine.

## GUC isolation (nuc, government query, single-run index, p50 ms)

| configuration | p50 ms | vs all-off |
|---------------|-------:|-----------:|
| all off (== v1.12.0)       | 201.99 | baseline |
| + coalesce_enable          | 201.83 | −0.1% |
| + coalesce + flush_to_run  | 201.83 | −0.1% |
| + all three (default-on)   | 202.97 | +0.5% |

The three features cost **~1 ms (0.5%) — pure noise** on a single-run index.
NOTE: the +18% I first reported was an artifact of meh's loaded 24-core box;
the clean nuc run shows no real cost.

## Conclusions (corrected by the clean nuc data)

1. **Accuracy: perfect**, cross-platform (FreeBSD + Linux), both versions. No
   correctness regression anywhere.
2. **Defaults-on is performance-neutral** on clean hardware (≤0.5% on single-run
   selective queries) — NOT the regression the noisy meh box suggested.
3. **But defaults-on shows no measured *benefit* on these workloads either**:
   the LSM/crack payoff requires multi-run indexes, which neither benchmark
   created (no post-build INSERT-driven merges). Coalescing gave no size win on
   this corpus.
4. **Net for the un-gate decision**: enabling the three by default is *safe*
   (neutral + accurate), but unproven-beneficial here. The honest call is to
   keep them **default-off** until a workload that exercises multi-run indexes
   (sustained INSERT + VACUUM) demonstrates the crack/flush benefit — at which
   point flipping is low-risk. The features are correct and ready; they just
   lack a benchmark that shows their upside.
5. **pg_tre is not a faster pg_trgm** (1.6–66× slower on shared queries, by
   design). Its differentiator is approximate edit-distance regex.
6. **Upgrade v1.6.0/v1.12.0 → v2.0.0-dev works** (CI-proven): old-format
   indexes read correctly after ALTER EXTENSION UPDATE, no REINDEX.

## Host coverage

- **nuc** (FreeBSD 15 amd64, PG18.4 from source): full A/B + GUC isolation.
  Authoritative (low variance). pg_tre + vendored TRE build cleanly on FreeBSD;
  the TRE autotools bootstrap was generated off-host with the nix flake toolchain
  and overlaid (portable text artifacts; config.guess detects the host).
- **meh** (Linux x86_64, PG18.4 via nix): full A/B; accuracy + ratios valid,
  absolute latencies noisy (shared box).
- **rv** (riscv64), **sun** (Solaris/SPARC): skipped (no PostgreSQL; separate
  portability bring-up — SPARC big-endian would stress the on-disk format).
