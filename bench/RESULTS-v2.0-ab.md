# pg_tre A/B benchmark results — v2.0 (defaults-on) vs v1.12.0 vs pg_trgm

Run: 2026-06-18, host **meh** (Linux 6.12 x86_64, 24 core), PostgreSQL 18.4,
self-contained ephemeral cluster (`bench/ab-bench.sh`). Corpus: 50,000 rows,
~150 B each, realistic selectivity (planted tokens at 5% / 1% / 0.08%).

Reproduce: `bench/ab-bench.sh --pg-config <pg18 pg_config> --src <checkout>
--corpus <csv> --work <scratch dir>`. Corpus: `bench/gen_corpus_realistic.py`.

## Build + size

| index | build time | size | vs pg_trgm |
|-------|-----------:|-----:|-----------:|
| pg_tre v2.0 (defaults-on) | 33.9 s | 296.4 MB | 24.3× larger |
| pg_tre v1.12.0            | 38.0 s | 297.2 MB | 24.4× larger |
| pg_trgm (GIN)             |  3.0 s |  12.2 MB | — |

- Coalescing (default-on in v2.0) did **not** shrink the index (296 vs 297 MB).
  On this corpus the medium bucket (serialized sparsemap 2048–3072 B) is nearly
  empty — postings are either tiny (stored inline) or huge (dedicated tree) —
  so coalescing has almost nothing to pack. The 24× size gap vs pg_trgm is the
  real story and is unaffected by coalescing.

## Accuracy (the oracle)

**Every query, both engines, both pg_tre versions: index result set ==
sequential-scan result set (0 mismatches).** pg_tre is exact (recheck is
authoritative); accuracy is not in question. This is the headline correctness
result of the whole exercise.

## Query latency — pg_tre v2.0 vs pg_trgm (p50 ms)

| query | selectivity | hits | pg_tre v2.0 | pg_trgm | ratio |
|-------|------------:|-----:|------------:|--------:|------:|
| rare `naturalize` (regex `~`) | 0.08% |   38 |    3.7 |  0.68 |  pg_trgm 5× |
| mid  `electrification`        | 1.0%  |  486 |  822.3 |  4.74 | pg_trgm 173× |
| common `government`           | 5.0%  | 2474 | 1510.6 | 15.56 | pg_trgm 97× |
| LIKE `%electrific%`           | 1.0%  |  488 |  400.7 |  2.38 | pg_trgm 168× |
| nonselective `the` (seqscan)  | 45%   |22513 |  506.8 | 44.48 | pg_trgm 11× |

**pg_trgm is dramatically faster on every shared query** (5×–170×). This is
expected and by design: pg_tre rechecks every candidate with the full TRE regex
engine, while pg_trgm's GIN bitmap + cheap recheck is far lighter for plain
substring/regex-literal matching. pg_tre's value is **approximate (edit-distance)
regex**, which pg_trgm cannot do at all (q5 below) — not raw speed on exact
literals. The north star holds: pg_tre is a regex index *with edit distance*,
not a faster pg_trgm.

pg_tre-only (no pg_trgm equivalent): `q5 approx1 govrnment` (k=1 fuzzy) =
2326 ms p50, 2477 hits, accuracy OK.

## Self comparison — v2.0 (defaults-on) vs v1.12.0 (p50 ms)

| query | v2.0 | v1.12.0 | delta |
|-------|-----:|--------:|------:|
| common `government`    | 1510.6 | 1211.4 | **+25% slower** |
| mid `electrification`  |  822.3 |  669.5 | **+23% slower** |
| rare `naturalize`      |    3.7 |    4.6 | −20% (faster) |
| LIKE `%electrific%`    |  400.7 |  547.8 | −27% (faster) |
| approx1 `govrnment`    | 2326.9 | 2078.3 | +12% slower |
| nonselective `the`     |  506.8 | 1526.9 | **−67% (3× faster)** |

Mixed: v2.0 wins big on the nonselective scan (the crack cache amortizes
repeated per-trigram union work across a large candidate set) and on LIKE, but
**regresses ~25% on selective queries** — the common case.

## GUC isolation (government query, single-run index)

| configuration | p50 ms | vs all-off |
|---------------|-------:|-----------:|
| all off (== v1.12.0)                 | 1605 | baseline |
| + coalesce_enable                    | 1646 | +2.6% |
| + coalesce + flush_to_run            | 1808 | +12.6% |
| + coalesce + flush + crack (default) | 1894 | +18.0% |

On a single-run index with selective queries, the three defaults-on features
add ~18% latency for **zero benefit**: there are no multiple LSM runs to
skip/crack, and no large scan to amortize the crack cache. The benefit only
materializes when an index actually has multiple runs (after INSERT-driven
merges with flush_to_run) or on nonselective scans.

## Conclusions

1. **Accuracy: perfect** across all queries, both versions, vs seq-scan. No
   correctness regression from the v2.0 work.
2. **Defaults-on is a net latency regression for the common case** (~18–25% on
   selective queries on a freshly-built single-run index). It only wins on
   nonselective scans (3×) and LIKE. **Recommendation: keep all three GUCs
   default-off** until: flush_to_run skips the multi-run overlay when n_runs==1;
   crack_on_read only allocates its cache for genuinely multi-run scans; and
   coalescing is either retuned or left off (it gave no size win here).
3. **pg_tre is not a faster pg_trgm** and was never meant to be — pg_trgm is
   5–170× faster on the shared exact/LIKE/regex-literal queries. pg_tre earns
   its place only via approximate (edit-distance) regex, which pg_trgm cannot
   express. Positioning must stay on that north star.
4. **Upgrade v1.12.0 → v2.0.0-dev works** (separately CI-proven): old-format
   indexes read correctly after ALTER EXTENSION UPDATE, no REINDEX.

## Host coverage

- **meh** (Linux x86_64, PG18.4): full A/B above. Authoritative.
- **nuc** (FreeBSD 15 amd64): PG18.4 built from source; **pg_tre C sources
  compile cleanly on FreeBSD** (a real portability data point), but the full
  A/B is blocked — the vendored TRE autotools bootstrap needs gettext +
  libtool dev tools that are not installed and cannot be added without root.
  nuc is also x86-64, so it adds no architectural diversity over meh.
- **rv** (riscv64), **sun** (Solaris/SPARC): no PostgreSQL; out of scope
  (separate portability bring-up — SPARC is big-endian and would stress the
  on-disk format, a distinct project).
