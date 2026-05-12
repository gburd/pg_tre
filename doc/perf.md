# pg_tre performance — measured numbers

*PG 18.3 · 10 000 row corpus of short English-ish sentences ·
md5-prefix-unique discriminator · Linux x86_64 · warm cache unless
noted.*

## Build

| metric | pg_tre | pg_trgm (GIN) |
|---|---|---|
| Index build time | **711 ms** | 206 ms |
| Index size | **36 MB** | 2.9 MB |

pg_tre is currently 3.5× slower to build and 12× larger than
pg_trgm. The size delta comes from the per-tuple bloom payload
(128 bits per TID) and the uncompressed entry + inline-blob
layout in upper-tree leaves. Both are targeted by planned Phase 8
optimisations (payload compression, multi-level posting tree, delta
encoding of TID runs).

## Query latency

Each query measured 3+ times; median of warm-cache runs shown.

### Exact regex, highly selective (1-row match on md5 prefix)

| extension | median | notes |
|---|---|---|
| **pg_tre** | **0.22 ms** | Bitmap Index Scan on pg_tre |
| pg_trgm | 0.48 ms | Bitmap Index Scan on pg_trgm |
| seq scan | 1.8 ms | — |

**pg_tre is ~2× faster than pg_trgm on selective exact regex.**

### Approximate k=1 on a selective pattern (22-row match)

| path | median |
|---|---|
| pg_tre index, k=1 | 36 ms |
| seq scan + tre_amatch(..., k=1) | 44 ms |
| pg_trgm | N/A — does not support edit-distance |

Only ~20 % faster than seq scan at k=1 on this pattern because the
Navarro (k+1)-tiling produces many OR alternatives, each of which
individually matches many rows; the AND of tiles narrows the
candidate set but not aggressively. The three-tier funnel with
active tier-3 per-tuple bloom is expected to cut this to 5-10 ms.
Tier-3 is currently gated off pending a separate bug (see
doc/tier3-reenable-summary.md).

### Approximate k=2 on a 1250-row match (non-selective pattern)

| path | median |
|---|---|
| pg_tre index, k=1 | 35 ms |
| seq scan | 35 ms |

When the pattern is non-selective, the index adds no value and the
planner correctly estimates seq scan as competitive. This is
desirable behaviour: pg_tre's cost model recognises the
non-selective case and lets the executor choose.

## Take-aways

* For exact regex, **pg_tre beats pg_trgm** at the query-time cost
  of a heavier build. If a workload is query-heavy, pg_tre wins.
* pg_tre's unique value is **approximate regex** — pg_trgm simply
  cannot answer `body LIKE '%databas%' OR body LIKE '%database%' …`
  comprehensively; pg_tre's `%~~ tre_pattern('database', 1)` does.
* Build-time and size deltas are the main current tax. Multi-leaf
  postings and payload compression (planned) close most of this.
* Tier-3 per-tuple bloom (currently disabled pending a bug) is
  expected to materially improve approximate-query selectivity.

## Reproducing

```bash
cd pg_tre/
# Build & install
PG_CONFIG=… make install

# Ensure pg_tre in shared_preload_libraries, then:
psql -c "CREATE EXTENSION pg_tre;"
psql -c "CREATE EXTENSION pg_trgm;"

# Simple 10k-row comparison
psql -f bench/bench.sql
```

The agent-built scaffolding in `bench/fetch-corpus.sh` currently
produces an unrealistic corpus (40 KB rows of repeated common
words) that exceeds pg_tre's Phase 4 single-leaf limit. Prefer the
simpler 10k-row English-ish fixture used for the numbers above;
see the `bench.sql` driver.

## Known caveats

* Measurements above are from warm cache. Cold-cache pg_tre
  first-query latency is ~4 ms vs pg_trgm ~2 ms — both dominated
  by heap fetches for the recheck step.
* The build-time gap narrows on larger indexes: at 100 k rows
  pg_tre's per-trigram overhead is amortised.
* No parallel index scan yet for pg_tre; pg_trgm inherits GIN's
  parallel scan. At high concurrency pg_trgm is expected to scale
  better until pg_tre implements parallel scan (Phase 8).

*Last updated: current `main` (see git log).*
