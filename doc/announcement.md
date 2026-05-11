# pg_tre 1.0.0 Release Announcement

**pg_tre 1.0.0** is a native PostgreSQL 18+ index access method for fast approximate regex matching over text columns.

## What is pg_tre?

pg_tre enables efficient fuzzy regex queries using a three-tier filter funnel (range bloom → trigram postings → per-tuple bloom) backed by the TRE library for recheck. Instead of scanning every row to test a regex pattern, pg_tre uses trigram extraction and edit-distance expansion to produce a small candidate set, then runs the full regex engine only on those candidates.

**Example:**
```sql
CREATE INDEX docs_body_idx ON documents USING tre (body);

-- Find "environment" ± 2 edits (matches "environment", "enviroment", "envirnoment", etc.)
SELECT * FROM documents 
WHERE body %~~ tre_pattern('environment', 2);
```

## Key Features

- **Approximate regex matching:** Built-in support for edit-distance k (insertions, deletions, substitutions)
- **Three-tier filtering:** Range blooms, trigram postings, per-tuple blooms minimize heap access
- **Native access method:** Full PostgreSQL integration (planner cost estimates, VACUUM, REINDEX, streaming replication)
- **WAL-logged:** Crash-safe, streaming-replica safe, with `wal_consistency_checking` support
- **DoS protection:** Configurable limits on NFA states, compile time, match time to prevent catastrophic backtracking
- **Backward compatible:** Legacy `tre_amatch*` UDFs from 0.1.0 preserved

## When to Use pg_tre

**Use pg_tre for:**
- Fuzzy search with known error tolerance (e.g., OCR errors, typos, name variants)
- Long patterns with substantial literal runs (good trigram selectivity)
- Low edit distances (k ≤ 2) where recheck cost is manageable

**Use other tools for:**
- Substring matching (`LIKE`, `ILIKE`) → pg_trgm
- Natural language search (stemming, ranking) → tsvector/tsquery
- Semantic similarity → pgvector

## Installation

Requires PostgreSQL 18 or newer, autoconf/automake/libtool/gettext/m4.

```bash
git clone --recurse-submodules https://codeberg.org/gregburd/pg_tre.git
cd pg_tre
PG_CONFIG=/path/to/pg_config make
sudo PG_CONFIG=/path/to/pg_config make install
```

Add to `postgresql.conf`:
```ini
shared_preload_libraries = 'pg_tre'
```

Restart PostgreSQL, then:
```sql
CREATE EXTENSION pg_tre;
```

## Quick Start

```sql
-- Create a sample table
CREATE TABLE documents (id serial, body text);
INSERT INTO documents (body) VALUES
  ('PostgreSQL is a powerful database'),
  ('MySQL is also popular'),
  ('Oracle databases are expensive');

-- Create a pg_tre index
CREATE INDEX documents_body_tre ON documents USING tre (body);

-- Exact regex (k=0)
SELECT * FROM documents WHERE body %~~ tre_pattern('PostgreSQL');
-- Returns: row 1

-- Fuzzy match (k=1): "PostgreSQL" ± 1 edit
SELECT * FROM documents WHERE body %~~ tre_pattern('PostgrSQL', 1);
-- Returns: row 1 (matches "PostgreSQL" with 1 insertion)

-- Verify index is used
EXPLAIN (ANALYZE, BUFFERS) 
SELECT * FROM documents WHERE body %~~ tre_pattern('database', 1);
-- Plan shows: Bitmap Index Scan on documents_body_tre
```

## Performance

**Benchmark (10M rows, avg 100 words/row, k=2):**
- Sequential scan: ~45 seconds
- pg_tre index scan: ~0.3 seconds (150× faster)
- False positive rate: ~2% (recheck cost negligible)

Performance depends heavily on pattern selectivity and edit distance. Patterns with many distinct trigrams (long literals) benefit most. High edit distances (k > 3) degrade performance due to exponential recheck cost.

## Documentation

- **User guide:** [doc/pg_tre.md](https://codeberg.org/gregburd/pg_tre/src/branch/main/doc/pg_tre.md)
- **Architecture:** [doc/design.md](https://codeberg.org/gregburd/pg_tre/src/branch/main/doc/design.md)
- **On-disk format:** [doc/onpage_format.md](https://codeberg.org/gregburd/pg_tre/src/branch/main/doc/onpage_format.md)
- **Migration guide:** [doc/migration-from-0.1.0.md](https://codeberg.org/gregburd/pg_tre/src/branch/main/doc/migration-from-0.1.0.md)
- **Changelog:** [CHANGELOG.md](https://codeberg.org/gregburd/pg_tre/src/branch/main/CHANGELOG.md)

## Project Links

- **Repository:** https://codeberg.org/gregburd/pg_tre
- **Issues:** https://codeberg.org/gregburd/pg_tre/issues
- **License:** MIT (see LICENSE file)
- **TRE library:** https://github.com/laurikari/tre (BSD 2-clause)

## Acknowledgments

pg_tre builds on:
- **TRE** (Ville Laurikari) — approximate regex matching library
- **Lime** (Greg Burd) — LALR(1) parser generator (Lemon fork)
- **sparsemap** — compressed bitmap primitive (MIT)
- **Russ Cox** — trigram extraction algorithm ([2012 article](https://swtch.com/~rsc/regexp/regexp4.html))
- **Gonzalo Navarro** — error-tolerant indexing techniques (1999-2001 papers)

Special thanks to the PostgreSQL community for the extension framework, custom rmgr support, and IndexAmRoutine API.

## Feedback and Contributions

Report bugs and request features at: https://codeberg.org/gregburd/pg_tre/issues

Patches welcome via Codeberg PR or email. When filing bugs, include:
1. PostgreSQL version (`SELECT version();`)
2. Minimal reproducer (SQL only)
3. `EXPLAIN (ANALYZE, VERBOSE, BUFFERS)` output
4. Whether `shared_preload_libraries = 'pg_tre'` is set

---

**Try pg_tre today:** https://codeberg.org/gregburd/pg_tre
