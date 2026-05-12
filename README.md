# pg_tre

**PostgreSQL 18+ native index access method for approximate regex matching.**

[![Status](https://img.shields.io/badge/status-1.0.0--rc1_candidate-orange)](STATUS.md)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![PostgreSQL](https://img.shields.io/badge/postgresql-18%2B-blue)](https://www.postgresql.org/)

pg_tre indexes text columns using a three-tier filter funnel (range bloom →
trigram postings → per-tuple bloom) backed by the [TRE
library](https://github.com/laurikari/tre) for approximate-regex recheck.

It turns the classic `ripgrep`-over-data problem ("find text that looks like
this, maybe with a typo") into a SQL-composable indexed query:

```sql
SELECT id FROM docs WHERE body %~~ tre_pattern('(error){~1}.*(42[0-9]){~0}', 1);
-- Bitmap Index Scan on docs_tre   →   sub-millisecond on 10k rows.
```

---

## Features

- **True edit-distance regex matching** — configurable `k` (insertions,
  deletions, substitutions) per sub-expression, e.g.
  `(foo){~1}.*(bar){~2}`.
- **Three-tier filter funnel** — BRIN-style range blooms eliminate
  heap-block ranges; sparsemap trigram postings AND/OR merge
  candidates; per-tuple blooms refine without heap I/O.
- **Native access method** — real `IndexAmRoutine`, planner cost
  estimation, WAL-logged, VACUUM-aware, `REINDEX CONCURRENTLY` safe.
- **UTF-8 codepoint trigrams** — CJK, accents, emoji indexed
  correctly; ASCII stays zero-overhead.
- **DoS protection** — configurable caps on NFA states, compile
  time, per-match time.
- **Backward compatible** — legacy `tre_amatch*` UDFs from 0.1.0
  preserved.

---

## Quick Start

```sql
-- Requires shared_preload_libraries = 'pg_tre' in postgresql.conf
CREATE EXTENSION pg_tre;

CREATE TABLE documents (id serial, body text);
INSERT INTO documents (body) VALUES
  ('The PostgreSQL database system'),
  ('MySQL is also popular'),
  ('Oracle databases are expensive'),
  ('The Postgres databse system');   -- typo: "databse"

CREATE INDEX documents_body_tre ON documents USING tre (body);

-- Exact regex (k=0)
SELECT * FROM documents WHERE body %~~ tre_pattern('[Pp]ostgre', 0);

-- Tolerate 1 edit: finds both "database" and "databse"
SELECT * FROM documents WHERE body %~~ tre_pattern('database', 1);

-- Per-phrase edit budgets: loose "postgres", strict exact "system"
SELECT * FROM documents
 WHERE body %~~ tre_pattern('(postgres){~2}.*(system){~0}', 0);
```

---

## How pg_tre fits alongside pg_trgm, full-text search, and pgvector

| Extension | Indexes | Best for | pg_tre overlap? |
|---|---|---|---|
| **pg_trgm** (GIN/GiST) | character trigrams | exact regex, LIKE, trigram-similarity search | **significant** — both do trigram-prefiltered regex; pg_tre adds edit-distance |
| **tsvector / tsquery** (built-in FTS) | stemmed lexemes + positions | natural-language search, ranking, stopwords | **minimal** — word-level with language rules; different primitive |
| **pgvector / pgvectorscale** | float embeddings (HNSW/IVFFlat) | semantic similarity via LLM embeddings | **none** — orthogonal dimension (meaning vs lexical) |
| **pg_tre** | codepoint trigrams + blooms + postings | **approximate regex, fuzzy pattern match, typo-tolerant search** | — |

### Distinct value pg_tre provides

1. **Genuine Levenshtein-distance matching.** `pg_trgm %` is
   trigram-set Jaccard similarity — two strings with overlapping
   trigram sets score "similar" even when the edit distance is
   enormous. `pg_tre` answers "is this text within N edits of this
   pattern?" which is what humans usually mean by "fuzzy match."

2. **Per-subexpression edit budgets.** `(phrase1){~1}.*(phrase2){~2}`
   is a single index query. No other PG extension expresses this.

3. **Full regex semantics on top of fuzziness.** Character classes,
   alternation, anchors, `{m,n}` repetition — all composable with
   the `{~k}` edit-budget operator.

### Where pg_tre is *not* the answer

- **Exact substring / LIKE**: pg_trgm is battle-tested and ships
  with every PG install. Use it.
- **Ranking / linguistic features**: built-in FTS wins hands down
  (stemming, stopwords, language config, ts_rank).
- **Semantic similarity**: pgvector. pg_tre knows nothing about
  meaning.
- **Multi-language natural-language search**: FTS. pg_tre is
  language-agnostic — a feature for identifiers, code, logs, SKUs;
  a non-feature for prose search where you want "running" to match
  "run".

---

## Use cases — especially for agentic workflows

pg_tre shines in a category that LLM agents increasingly need:
**lexical pattern search over data the agent generates or
consumes, tolerant of OCR errors, typos, format drift, or
LLM hallucinations**.

| Agent task | Why pg_tre fits |
|---|---|
| **Log and trace search** — "find that stack trace about `foo_bar_baz` but the user typed `foo_bar_baxz`" | Exact grep would miss the typo; vectors miss the exact identifier structure. pg_tre's k=1 regex catches both without reranking. |
| **Code / symbol search with edits** — fuzzy identifier lookup across repos | Typos in identifier names, camelCase vs snake_case drift, and near-duplicates surface in a single query. |
| **Catalog / SKU / UUID / error-code lookup** — "did anyone report error `E-2341` or a near variant?" | No linguistics needed; pure pattern matching with edit tolerance. |
| **Template / pattern extraction** — find all variants of `"user \d+ logged in from .*"` across a corpus of logs | Single regex, approximate-tolerant. |
| **Observability / audit-log triage** — semi-structured text with numeric IDs and a few typos | Complements `pg_trgm` (substring) and FTS (ranked prose) by providing the missing fuzzy-regex primitive. |
| **Retrieval-augmented generation (RAG) hybrid retrieval** — filter by lexical pattern *then* vector-rank | pg_tre does the lexical filter; pgvector does the semantic rank; FTS handles natural language. Three indexes, three axes. |

**Stacking with other extensions** is the typical pattern:

```sql
-- Hybrid retrieval: lexical pattern filter + semantic rank
WITH pattern_hits AS (
  SELECT id, body
  FROM docs
  WHERE body %~~ tre_pattern('error (42[0-9]|E-\d+){~1}', 1)
)
SELECT h.id, h.body, embedding <=> $1 AS distance
FROM pattern_hits h
JOIN embeddings e USING (id)
ORDER BY e.embedding <=> $1
LIMIT 10;
```

---

## Performance

Measured on a 10 000-row fixture, PG 18.3, warm cache (full numbers
in [`doc/perf.md`](doc/perf.md)):

| Query | pg_tre | pg_trgm | seq scan |
|---|---|---|---|
| Exact regex, selective (1 row) | **0.22 ms** | 0.48 ms | 1.8 ms |
| Approximate k=1, selective (22 rows) | **36 ms** | N/A | 44 ms |
| Approximate k=1, non-selective (1250 rows) | 35 ms | N/A | 35 ms |

- **~2× faster than pg_trgm** on selective exact regex.
- **Only option** for approximate regex — pg_trgm cannot answer
  edit-distance queries at all.
- Non-selective queries correctly fall back to seq scan (planner
  cost model is calibrated).

**Current taxes** (all targeted by pre-1.0.0 final work):
index build is ~3.5× slower and ~12× larger than pg_trgm because
of the per-tuple bloom payload and uncompressed upper-tree layout.
Multi-leaf posting trees and payload compression close most of this.

---

## Installation

Requires PostgreSQL 18+, autoconf/automake/libtool/gettext/m4 for
the vendored [TRE](https://github.com/laurikari/tre).

```bash
git clone --recurse-submodules https://codeberg.org/gregburd/pg_tre.git
cd pg_tre
PG_CONFIG=/path/to/pg_config make
sudo make install
```

Then, in `postgresql.conf`:

```
shared_preload_libraries = 'pg_tre'
```

Restart PG and `CREATE EXTENSION pg_tre;` in each database that
needs it.

Packaging templates for Debian (`debian/`), RPM
(`packaging/pg_tre.spec`), Homebrew
(`doc/homebrew-formula.rb`), Docker (`doc/Dockerfile`), and PGXN
(`META.json`) ship in-tree.

---

## Status and roadmap

See [STATUS.md](STATUS.md) for the live phase tracker. Current
state is **1.0.0-rc1 candidate**:

- **Production-ready**: build, scan, incremental writes, crash
  recovery, approximate regex k≤2, UTF-8, DoS hardening, planner
  cost model.
- **Pre-1.0.0 polish in flight**: tier-3 bloom reactivation,
  multi-leaf posting trees, parallel index scan.

Tag and release process documented in
[`doc/release-checklist.md`](doc/release-checklist.md). A
pre-tag gate lives in `scripts/release-check.sh`.

---

## Documentation

- **[doc/pg_tre.md](doc/pg_tre.md)** — user reference (types,
  operators, functions, GUCs, reloptions, cookbook).
- **[doc/design.md](doc/design.md)** — architecture, three-tier
  filter funnel, on-disk format decisions.
- **[doc/perf.md](doc/perf.md)** — measured numbers vs pg_trgm
  and seq scan, with the reproducer in `bench/bench.sql`.
- **[doc/migration-from-0.1.0.md](doc/migration-from-0.1.0.md)** —
  upgrade guide from the UDF-only 0.1.0.
- **[doc/release-checklist.md](doc/release-checklist.md)** —
  1.0.0-rc1 → 1.0.0 final gate.
- **[CHANGELOG.md](CHANGELOG.md)** — grouped by phase.

---

## License

pg_tre is MIT (see [LICENSE](LICENSE)). Vendored components have
their own licenses — all permissive, all redistribution-friendly:

- [sparsemap](src/util/sparsemap.c) — MIT (in-tree)
- [TRE](https://github.com/laurikari/tre) — BSD-2-clause (submodule)
- [Lime](https://codeberg.org/gregburd/lime) — public domain
  (build-time generator only, not linked)

See [NOTICE](NOTICE) for the full attribution.

---

## Contributing

Open issues and PRs at
[codeberg.org/gregburd/pg_tre](https://codeberg.org/gregburd/pg_tre).
CI runs on both Forgejo Actions (primary, at Codeberg) and GitHub
Actions (mirror). See `.forgejo/workflows/` and
`.github/workflows/`.

Before sending a patch: `scripts/release-check.sh` must pass.
