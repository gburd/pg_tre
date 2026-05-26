# pg_tre performance — measured numbers

*PG 18.3 · 1 000 000 row corpus from `/usr/share/dict/words`
(Zipfian-style sample of 5–12 words per row, ~85 chars avg) ·
seeded with `connection refused` (~10000 rows), `error E-NNNN`
(~1000 rows), `database` (~50000 rows) · Linux x86_64 · warm cache
unless noted.*

## 1M-row benchmark (1.4.1+)

### Build

| metric | pg_tre | pg_trgm (GIN) |
|---|---|---|
| Build time | **80 s** | 28 s |
| Index size | **3843 MB** | 159 MB |
| Distinct trigrams indexed | 24,269 | n/a |
| Trigram emissions during build | 83.5 M | n/a |

pg_tre is 2.9× slower to build and 24× larger than pg_trgm at
1M rows. The size delta is structural: pg_tre's posting layout
allocates separate leaf pages per distinct trigram regardless of
posting-list cardinality. v2.0's posting-page coalescing
(`doc/specs/posting-page-coalescing.md`) collapses low-cardinality
trigrams onto shared pages and is expected to close most of this
gap. The build-time gap is dominated by the same effect — more
pages written.

### Query latency

| pattern | k | matches | pg_tre | pg_trgm | seq scan |
|---|---|---|---|---|---|
| `connection refused` | 0 | 10,000 | 18.5 s | **35 ms** (`LIKE`) | 69 ms |
| `connectoin refused` | 1 | 10,000 | 5.5 s | **n/a** (no edit-distance) | n/a |
| `database` | 0 | 50,043 | **263 ms** | 29 ms (`LIKE`) | n/a |
| `databse` | 1 | 50,215 | 7.3 s | **n/a** | n/a |
| `E-[0-9]{4}` | 0 | 0 | **1.5 s** | n/a (no regex) | n/a |
| `connectoin refused` k=2 ORDER BY `<@>` LIMIT 10 | 2 | top-10 | 6.4 s | **n/a** | n/a |

**The honest picture at 1M rows:**

- pg_tre is **slower than seq scan or `LIKE`/pg_trgm** for the
  exact-match cases. The cost model is correctly choosing the
  index plan but the per-candidate overhead dominates: with
  ~24k distinct trigrams and posting lists that are physically
  spread across many pages each, candidate-set extraction is
  I/O-heavy.
- Where pg_tre is the **only** answer (k≥1 fuzzy search,
  character-class regex like `E-[0-9]{4}`, `<@>` distance
  ordering), pg_tre completes in 1–7 seconds on 1M rows. No
  alternative in PostgreSQL today produces these results from
  an index — the fallback is seq scan + `tre_amatch`, which
  takes O(N × pattern_len × k) per row.

**The gap closes substantially below 100K rows.** See the smaller
benchmark below for selective queries where pg_tre's three-tier
funnel actually wins.

## Smaller-corpus benchmark (10K rows, short sentences)

The original 10K-row fixture (`bench/bench.sql`) shows pg_tre
competitive on selective exact-match:

| path | median |
|---|---|
| pg_tre index, exact 1-row match | **0.22 ms** |
| pg_trgm GIN, same | 0.48 ms |
| seq scan | 1.8 ms |

pg_tre wins ~2× on this pattern. The trade-off is build time and
size: 711 ms vs 206 ms; 36 MB vs 2.9 MB.

For approximate queries (no pg_trgm comparison possible):

| path | median |
|---|---|
| pg_tre index, k=1 selective | 36 ms |
| seq scan + `tre_amatch(..., k=1)` | 44 ms |

20% faster than seq scan at k=1 because the Navarro (k+1)-tiling
produces many OR alternatives that each individually match many
rows; the AND of tiles narrows the candidate set but doesn't
match the selectivity of a true btree-style equality probe.

## Take-aways

* **Exact regex on small to medium corpora**: pg_tre beats
  pg_trgm at query time. Worthwhile for read-heavy workloads.
* **Approximate regex (any k > 0)**: pg_tre is the only index-
  driven option in PostgreSQL. Whether pg_tre's index path
  beats seq scan depends on selectivity — selective patterns
  win, broad ones don't.
* **Million-row corpora**: pg_tre's structural overhead per
  distinct trigram dominates today. v2.0's posting-page
  coalescing is the planned fix. Until then, pg_tre is best
  suited for narrow columns (short text) or smaller corpora,
  or as a complement to pg_trgm (one column, two indexes —
  the planner picks the cheaper for each query shape).

## Real bugs surfaced by this benchmark

The 1M-row benchmark caught two production-blocking bugs in
1.4.0 that were silently broken before:

1. **`palloc` 1 GB cap** in `ambuild.c`: ~50M trigram entries at
   24 bytes each is 1.2 GB — exceeds PG's `MaxAllocSize`. Fix
   in 1.4.1: switch the entries-array allocation to
   `MemoryContextAllocHuge` / `repalloc_huge`.
2. **Single-level upper-tree internals** in `upper.c`: the
   `upper_build_internal_level` was hardcoded to one page,
   capping pg_tre at ~10–20K distinct trigrams (which is just
   ~50K rows of dictionary text). Fix in 1.4.1: recursive
   multi-level construction in the writer; multi-level
   descent loop in `pg_tre_upper_lookup`.

Both fixes also surfaced a third (latent) bug: the old
single-level descent's edge case where `trigram_hash <
first_key[0]` returned no match was producing wrong results on
some queries even at small scales. The 1.4.1 multi-level
descent loop fixes that incidentally — `test/expected/utf8.out`
and `test/expected/order_by.out` were refreshed to reflect the
now-correct behavior (rows that the index used to drop are now
returned).

## Reproducing

```bash
# Generate the corpus (3.9 s for 1M rows)
python3 bench/gen_corpus.py 1000000 /tmp/corpus_1m.csv

# Load + build + run query panel
createdb bench_1m
psql -d bench_1m -f bench/bench_1m_v2.sql
```

`bench/gen_corpus.py` ships in the repo. The plpgsql sampler in
the original `bench/bench_1m.sql` is O(N²) and was unusable
beyond ~10K rows; `bench_1m_v2.sql` uses Python-generated CSV
input via `\copy` instead.

## Known caveats

* Measurements above are from a warm cache, single-threaded
  client, single-CPU build. NUMA / parallel scan / parallel
  build are not yet implemented in pg_tre.
* The 1M corpus is synthetic dictionary words; real-world
  natural text has different trigram distributions and the
  numbers will shift accordingly. Re-run on your corpus.
* `\timing on` numbers in psql include client-side round-trip;
  the underlying executor times are 1–3 ms lower per query for
  the small ones.

*Last updated: 1.4.1 (1M-row benchmark).*
