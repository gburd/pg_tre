# pg_tre Benchmark Harness

This benchmark compares pg_tre against pg_trgm (closest existing alternative) with measured numbers for doc/perf.md.

## Quick Start

```bash
# 1. Ensure PostgreSQL 18+ is running with pg_tre and pg_trgm loaded
# postgresql.conf must have: shared_preload_libraries = 'pg_tre'

# 2. Set your pg_config path
export PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config

# 3. Generate corpus
./fetch-corpus.sh

# 4. Load data and build indexes
./load-and-index.sh

# 5. Run queries
./run-queries.sh

# 6. Generate performance report
./report.sh
```

This produces `doc/perf.md` with real benchmark numbers.

## Scripts

- **fetch-corpus.sh** — Generates deterministic synthetic corpus (10k rows, ~200 chars each)
- **load-and-index.sh** — Creates bench_tre and bench_trgm tables, builds indexes, measures build time
- **run-queries.sh** — Runs query matrix (exact, k=1, k=2, multi-phrase), measures latency
- **report.sh** — Aggregates results into doc/perf.md

## Corpus

The synthetic corpus:
- 10,000 rows of ~200-character text
- Built from 10,000 English words (deterministic vocabulary)
- ~2% intentional typos for fuzzy-match realism
- Fixed seed for reproducibility
- Saved to `bench/corpus.csv`

## Query Matrix

| Query Type | Pattern | pg_tre | pg_trgm | Notes |
|------------|---------|--------|---------|-------|
| Exact | `lexicon` | ✓ | ✓ | Both use index |
| Exact | `electrification` | ✓ | ✓ | Long word |
| Exact | `obscure_rare_01234` | ✓ | ✓ | Rare pattern |
| Approx k=1 | `lexcon` (1 typo) | ✓ | N/A | pg_tre only |
| Approx k=2 | `lexco` (2 typos) | ✓ | N/A | pg_tre only |
| Multi-phrase | `(foo){~1}.*(bar){~2}` | ✓ | N/A | pg_tre only |
| Non-selective | `the`, `a` | ✓ | ✓ | Seq-scan expected |

## Metrics

For each query:
- **Hits:** Row count
- **Build time:** Index creation time (ms)
- **Index size:** pg_relation_size (MB)
- **Query latency:** p50/p95/p99 over 10 runs (ms)
- **EXPLAIN output:** Saved to `bench/results/<query-id>.txt`

## Known Limitations

- **Single-leaf posting cap:** Very common trigrams (e.g., "the", "ing") may hit the ~8 KB limit. If this occurs, the benchmark will document it in doc/perf.md rather than failing.
- **Planner may pick seq-scan:** For non-selective queries, this is expected. The benchmark measures both with and without `SET enable_seqscan=off`.
- **Tier-3 filter disabled:** Current implementation. Potential future speedup noted in doc/perf.md.

## Environment

- PostgreSQL 18.3 at `~/.pgrx/18.3/pgrx-install/bin/pg_config`
- Database: `bench_db` (created by load-and-index.sh)
- Socket: `/tmp` (PGHOST=/tmp)
- Extensions: pg_tre, pg_trgm

## Output

- `bench/corpus.csv` — Raw corpus data
- `bench/results/build_times.txt` — Index build measurements
- `bench/results/query_*.txt` — Per-query EXPLAIN ANALYZE output
- `bench/results/timings.csv` — Raw timing data
- `doc/perf.md` — Final performance report
