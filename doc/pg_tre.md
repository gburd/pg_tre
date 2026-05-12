# pg_tre User Guide

**pg_tre** is a PostgreSQL 18+ native index access method for fast approximate regex matching over text columns. It uses a three-tier filter funnel (range bloom → posting tree → per-tuple bloom) backed by the [TRE library](https://github.com/laurikari/tre) for approximate pattern recheck.

---

## Introduction

### What pg_tre Does

pg_tre indexes text columns to enable efficient approximate regex queries of the form:

```sql
SELECT * FROM documents
WHERE body %~~ tre_pattern('enviro.{~2}ment', 2);
```

This query finds rows where `body` contains a word within edit-distance 2 of the literal pattern `environment` (e.g., "environment", "enviroment", "envirnoment", etc.). Without an index, PostgreSQL must scan every row and run the regex engine on each; with pg_tre, only candidate rows are examined.

### When to Use pg_tre

**Use pg_tre when:**
- You need approximate regex matching (fuzzy search with edit distance k > 0)
- Patterns contain substantial literal runs (3+ character substrings)
- Low edit distances (k ≤ 3) — recheck cost grows exponentially with k
- Selective patterns (matches < 10% of rows) — high selectivity benefits from index filtering

**Use pg_trgm when:**
- You need similarity search (`%`, `<->`) or exact substring matching (`LIKE`, `ILIKE`)
- No regex syntax required

**Use full-text search (tsvector/tsquery) when:**
- You need linguistic analysis (stemming, stop words, ranking)
- Natural language queries over structured documents

**Use pgvector when:**
- You need semantic similarity (embedding-based search)

**Use sequential scan when:**
- Patterns lack literals (e.g., `.*foo.*` where `foo` is the only trigram)
- Very high edit distances (k > 3) — recheck dominates cost
- Low selectivity (matches > 50% of rows)

### Performance Characteristics

**pg_tre wins when:**
- Long patterns with multiple literals: `environment.*database.*configuration` (many trigrams → high selectivity)
- Low k (0-2): recheck is fast
- Common trigrams appear in distinct positions: tiling partitions the pattern space effectively

**Sequential scan wins when:**
- Short patterns: `a.{~1}b` (only 2 trigrams → poor selectivity)
- High k (> 3): recheck cost dominates
- Non-literal regex: `[a-z]+@[a-z]+\.(com|org)` (few usable trigrams)

**Rule of thumb:** If your pattern has ≥ 10 distinct trigrams and k ≤ 2, pg_tre likely helps. Use `EXPLAIN ANALYZE` to verify.

---

## Installation

### Requirements

- **PostgreSQL 18 or newer**
- **Build tools:** gcc/clang, make, autoconf, automake, libtool, gettext, m4
- **Git submodules:** TRE (v0.9.0) and Lime parser generator

### Build

```bash
# Clone with submodules
git clone --recurse-submodules https://codeberg.org/gregburd/pg_tre.git
cd pg_tre

# Build and install
PG_CONFIG=/path/to/pg_config make
sudo PG_CONFIG=/path/to/pg_config make install
```

If you cloned without `--recurse-submodules`:
```bash
git submodule update --init --recursive
```

### Enable the Extension

**Critical:** pg_tre requires `shared_preload_libraries` for its custom WAL resource manager:

```ini
# postgresql.conf
shared_preload_libraries = 'pg_tre'
```

Restart PostgreSQL:
```bash
pg_ctl restart -D /path/to/datadir
```

Then in your database:
```sql
CREATE EXTENSION pg_tre;
```

**Without preload:** The legacy UDFs (`tre_amatch*`, `tre_version`) work, but `CREATE INDEX USING tre` will fail.

---

## Reference

### Types

#### tre_pattern

Compiled regex pattern with approximate-match metadata.

**Constructors:**
```sql
tre_pattern(pattern text) → tre_pattern
  -- Creates pattern with default max_cost (GUC pg_tre.default_max_cost, default 3)

tre_pattern(pattern text, max_cost int) → tre_pattern
  -- Creates pattern with explicit max edit-distance budget

tre_pattern(pattern text, max_cost int, cost_ins int, cost_del int, cost_subst int) → tre_pattern
  -- Creates pattern with custom per-operation costs
```

**Grammar:** Standard POSIX extended regex (ERE) with TRE approximate-match extension `{~m}`:
- `.` — any character
- `*` `+` `?` — repetition
- `[abc]` `[^abc]` — character classes
- `(foo|bar)` — alternation
- `^` `$` — anchors
- `{~m}` — approximate block: match preceding atom with up to m edits

**Examples:**
```sql
-- Exact match (k=0)
tre_pattern('hello')

-- Fuzzy match: "hello" ± 1 edit
tre_pattern('hello', 1)

-- Local budget: "environment" ± 2 edits, rest exact
tre_pattern('enviro.{~2}ment.*database')

-- Custom costs: deletions cost 2, others cost 1
tre_pattern('config', 3, 1, 2, 1)
```

### Operators

#### %~~ (approximate regex match)

```sql
text %~~ tre_pattern → boolean
```

Returns true if the text matches the pattern within the edit-distance budget.

**Indexable:** When used in a WHERE clause, the planner may choose a Bitmap Index Scan on a `tre` index.

**Example:**
```sql
SELECT * FROM docs WHERE body %~~ tre_pattern('PostgreSQL', 1);
-- Matches: "PostgreSQL", "PostgeSQL", "PotsgreSQL", etc.
```

### Functions

#### Legacy UDFs (0.1.0 compatibility)

These existed before the index AM and remain for backward compatibility:

```sql
tre_amatch(input text, pattern text, max_cost int) → boolean
  -- Approximate match with default costs (1,1,1)

tre_amatch_cost(input text, pattern text, max_cost int) → int
  -- Returns edit distance if matched, else NULL

tre_amatch(input text, pattern text, max_cost int, 
           cost_ins int, cost_del int, cost_subst int) → boolean
  -- Approximate match with custom costs

tre_amatch_detail(input text, pattern text, max_cost int) 
  → TABLE(cost int, num_ins int, num_del int, num_subst int, 
          match_start int, match_end int)
  -- Returns detailed match information (single row)
```

**Note:** These do NOT use the index; they always run TRE's regex engine. Use the `%~~` operator for index scans.

#### Introspection

```sql
tre_version() → text
  -- Returns TRE library version (e.g., "TRE 0.9.0")

tre_parse_debug(pattern text) → text
  -- Returns AST of parsed regex (for debugging extraction)

tre_extract_debug(pattern text) → text
  -- Shows trigram extraction output (debugging planner)
```

### Access Method

#### Creating Indexes

```sql
CREATE INDEX idx_name ON table_name USING tre (column_name);
```

**Limitations:**
- Single-column indexes only (`amcanmulticol = false`)
- Text columns only (opclass `tre_text_ops`)
- Lossy (no index-only scans; recheck always required)

#### Storage Options (WITH clause)

```sql
CREATE INDEX idx_name ON table_name USING tre (column_name)
WITH (
  fastupdate = true,              -- Enable pending list (default: true)
  pending_list_limit = 4096,      -- Pending list size in KiB (default: 4096)
  bloom_tuple_bits = 128,         -- Per-tuple bloom size (default: 128)
  range_size_blocks = 128,        -- Blocks per range entry (default: 128)
  tuple_bloom_enable = true       -- Enable tier-3 blooms (default: true)
);
```

**fastupdate:** When true, INSERTs append to a pending list; VACUUM merges them into the main tree. Improves write throughput at the cost of slower scans until merge.

**pending_list_limit:** Maximum pending list size in KiB before auto-merge. Larger = better write throughput, slower unmaintained scans.

**bloom_tuple_bits:** Bits per per-tuple bloom filter. More bits = lower false-positive rate = fewer heap fetches. Range: 32-1024.

**range_size_blocks:** Heap blocks summarized by each range bloom entry. Smaller = finer-grained tier-1 filtering, larger meta page.

**tuple_bloom_enable:** Disable to save space if tier-3 filtering doesn't help your workload.

### GUCs (Configuration Variables)

All GUCs use the `pg_tre.` prefix.

#### Query Behavior

```sql
SET pg_tre.default_max_cost = 3;  -- Default edit distance when unspecified
SET pg_tre.max_extraction_fanout = 256;  -- Max trigram disjuncts per query
```

#### Safety Limits (DoS Protection)

```sql
SET pg_tre.max_nfa_states = 10000;      -- Reject patterns with > N NFA states
SET pg_tre.compile_timeout_ms = 1000;   -- Abort regex compilation after N ms
SET pg_tre.match_timeout_ms = 1000;     -- Abort per-row recheck after N ms
```

These prevent catastrophic backtracking and runaway regex compilation. If you hit these limits legitimately, increase them; if you hit them on user input, the pattern is malicious or pathological.

#### Index Build Defaults (apply when WITH options unset)

```sql
SET pg_tre.pending_list_limit = 4096;    -- KiB
SET pg_tre.range_size_blocks = 128;      -- heap blocks
SET pg_tre.bloom_tuple_bits = 128;       -- bits
SET pg_tre.fastupdate = true;
SET pg_tre.tuple_bloom_enable = true;
```

Context: `PGC_USERSET` (can set per-session), except `range_size_blocks` and `bloom_tuple_bits` are `PGC_SIGHUP` (require reload).

---

## Usage Cookbook

### 1. Exact Regex (k=0)

```sql
CREATE TABLE docs (id serial, body text);
INSERT INTO docs (body) VALUES 
  ('The PostgreSQL database'),
  ('MySQL is popular'),
  ('Oracle databases are expensive');

CREATE INDEX docs_tre_idx ON docs USING tre (body);

-- Find rows containing "PostgreSQL" (case-sensitive)
SELECT * FROM docs WHERE body %~~ tre_pattern('PostgreSQL');
-- Returns: row 1

EXPLAIN (ANALYZE, BUFFERS) 
SELECT * FROM docs WHERE body %~~ tre_pattern('PostgreSQL');
-- Plan: Bitmap Index Scan on docs_tre_idx
--       Recheck Cond: (body %~~ 'PostgreSQL'::tre_pattern)
```

**Why it works:** Pattern "PostgreSQL" yields trigrams `Pos`, `ost`, `stg`, ..., `SQL`. All present in row 1, absent in rows 2-3. Tier-2 posting merge produces TID set `{1}`, tier-3 bloom confirms, recheck validates.

### 2. Fuzzy Match (k=1)

```sql
-- Find "colour" or "color" (1 edit)
SELECT * FROM docs WHERE body %~~ tre_pattern('colo.?ur', 1);
-- Matches: "colour", "color"

-- Edit-distance expansion:
--   Trigrams extracted: {col, olo, lou, our} OR {col, olo, lor}
--   k=1 expansion via universal Levenshtein adds variants:
--     col → {col, xol, cxl, col, ...} (255 substitutions + insertions + deletions)
--   Planner chooses based on estimated selectivity.
```

### 3. When Seq Scan Wins

```sql
-- Pattern: ".*environment.*" (k=2)
SELECT * FROM docs WHERE body %~~ tre_pattern('.*environment.*', 2);

EXPLAIN SELECT * FROM docs WHERE body %~~ tre_pattern('.*environment.*', 2);
-- Plan: Seq Scan on docs
--       Filter: (body %~~ '.*environment.*'::tre_pattern)

-- Reason: Leading `.*` is non-selective; tiling extracts trigrams from
--         "environment" but the pattern matches anywhere in the text.
--         Planner estimates high row count → seq scan cheaper.
```

To make this index-scannable, anchor the pattern or add more literals:
```sql
-- Anchored: must start with "environment"
WHERE body %~~ tre_pattern('^environment', 2);

-- Additional context
WHERE body %~~ tre_pattern('.*environment.*database', 2);
```

### 4. Pending List Maintenance

```sql
-- Check pending list size
SELECT pg_relation_size('docs_tre_idx');  -- before
INSERT INTO docs (body) SELECT 'test' || i FROM generate_series(1, 10000) i;
SELECT pg_relation_size('docs_tre_idx');  -- after (pending list grew)

-- Drain pending list
VACUUM docs;
SELECT pg_relation_size('docs_tre_idx');  -- merged (may grow or shrink)

EXPLAIN (ANALYZE, BUFFERS) 
SELECT * FROM docs WHERE body %~~ tre_pattern('test123');
-- Before VACUUM: slower (pending list overlay)
-- After VACUUM: faster (posting tree only)
```

### 5. Approximate Match with Local Budget

```sql
-- "environment" ± 2 edits, rest exact
SELECT * FROM docs 
WHERE body %~~ tre_pattern('enviro.{~2}ment.*database');

-- Matches:
--   "environment management database"
--   "enviroment setup database"
--   "envirnoment config database"
-- Does NOT match:
--   "environment management MySQL"  (lacks "database")
```

**How it works:** The `{~2}` block applies locally to the preceding pattern slice. Tiling extracts trigrams from "enviro", "ment", "database" and expands only the "enviro...ment" portion by k=2.

---

## Performance Notes

**For measured benchmark numbers, see [perf.md](perf.md).**

This section describes the theoretical performance characteristics of pg_tre's three-tier filter architecture. Real measurements are in `doc/perf.md` once the Phase 5 ambuild bug is resolved.

### Three-Tier Filter Funnel

pg_tre uses three progressively refined filters before heap recheck:

1. **Tier 1 (Range bloom):** Groups heap blocks into ranges (default 128 blocks). Each range has a bloom filter of all trigrams in that region. Query trigrams tested against range blooms; entire ranges rejected if blooms don't match.

2. **Tier 2 (Posting tree):** Per-trigram sparsemap postings. AND/OR merged based on query mode (CNF for k=0, DNF for k>0 tiled). Produces candidate TID set.

3. **Tier 3 (Per-tuple bloom):** Each posting leaf stores a 128-bit bloom per TID with all trigrams from that tuple. Candidate TIDs tested; non-matches rejected without heap I/O.

4. **Recheck:** Surviving TIDs fetched from heap, TRE's `regaexec` runs the full approximate-match algorithm.

**False positive rate:** Tier-3 bloom has ~2% FPR at 10 trigrams/tuple. Recheck is mandatory (the index is lossy).

### Why Recheck is Necessary

The index stores trigrams, not the full text. Even exact regex matches require recheck because:
- Trigram presence doesn't prove ordering (e.g., trigrams `abc`, `bcd`, `cde` could be "abcde" or "cdeabc")
- Approximate matches require NFA simulation for edit-distance computation
- Anchors (`^`, `$`) and boundaries (`\b`) are not indexed

The recheck cost is why high k (> 3) degrades performance: TRE's approximate-match algorithm is exponential in k.

### Planner Cost Estimates

The planner uses `pg_tre_amcostestimate` to decide between index scan and seq scan:

- **Selectivity:** Per-trigram cardinalities from the meta page → estimated candidate rows
- **Index cost:** posting lookup + bloom checks + recheck
- **Seq scan cost:** scan all rows + recheck all

For k=0, selectivity is good (literal trigrams are precise). For k>0, tiling expands to DNF with k+1 tiles, each tile is a conjunction; the planner ORs their selectivities.

**If the planner always chooses seq scan:** Your pattern is non-selective. Try `SET enable_seqscan = off;` to force index scan and compare `EXPLAIN ANALYZE` costs.

### Debugging Selectivity

```sql
-- Show extracted trigrams and estimated fanout
SELECT tre_extract_debug('environment.*database');
-- Output: CNF mode, trigrams: {env, nvi, vir, ..., ase}, fanout: 18

-- Show parsed AST
SELECT tre_parse_debug('enviro.{~2}ment');
-- Output: CONCAT(CONCAT(Literal('enviro'), APPROX(..., k=2)), Literal('ment'))
```

---

## Known Limitations

### Phase 4 Single-Leaf Posting Budget

**Symptom:** `ERROR: posting for trigram ... exceeds single-leaf budget`

**Cause:** Very common trigrams (e.g., "the", "ing") generate postings > 7 KB. Phase 4's single-leaf implementation can't split them.

**Workaround:** Shorten the text, filter common trigrams, or REINDEX after Phase 8 ships multi-leaf posting splits.

**Status:** Deferred to Phase 8 (multi-level posting trees).

### UTF-8 Support

**Current:** Trigrams are extracted as byte-sequences. ASCII works perfectly. Multi-byte UTF-8 characters (e.g., "résumé") work but aren't optimal:
- Byte-trigrams cross character boundaries
- Selectivity estimates degrade for non-ASCII text

**Planned:** Phase 8 will add `tri_encoding = codepoint_hash` reloption for proper Unicode normalization.

### Range Bloom and Positional Filters

**Status (Phase 5):** Tier-1 range bloom and positional offsets are implemented but selectivity benefits are less than design intent.

- **Range bloom:** Works; current implementation is a single-leaf page (no binary search yet). For very large indexes (> 100 GB), range bloom lookup becomes linear. Phase 8 will extend to multi-level tree.
- **Positional filtering:** Positions are stored but not yet used in tier-2 filtering. Phase 5.1 will wire `sparsemap_offset` for +/- k shifts.

### DoS Protections

**Limits enforced:**

- `pg_tre.max_nfa_states`: Rejects patterns whose TRE-compiled NFA exceeds this state count. Prevents stack overflow.
- `pg_tre.compile_timeout_ms`: Aborts regex compilation after timeout. Prevents pathological patterns (e.g., nested quantifiers) from hanging.
- `pg_tre.match_timeout_ms`: Aborts per-row recheck after timeout. Prevents catastrophic backtracking.

**User-visible errors:**
```sql
ERROR:  regex too complex (estimated NFA states exceed pg_tre.max_nfa_states)
HINT:  Simplify the pattern or increase pg_tre.max_nfa_states.
```

**If you hit these limits:**
- For legitimate patterns: increase the GUC
- For user input: the pattern is malicious or too complex; reject it

### Approximate Match Recheck Cost

TRE's `regaexec` approximate-match algorithm is O(n * m * k^2) where n = text length, m = pattern length, k = edit distance. For k > 3, recheck dominates scan cost.

**Recommendation:** Use k ≤ 2 for production. For k = 3, test on your workload. Avoid k > 3 unless texts are very short.

---

## Troubleshooting

### Error: "posting for trigram ... exceeds single-leaf budget"

**Fix:** REINDEX after Phase 8 ships, or filter common words before indexing.

**Explanation:** Phase 4's posting tree is single-leaf. A single trigram posting must fit in ~7 KB. If you index 100k rows containing "the", the posting's sparsemap exceeds this.

**Temporary workaround:**
```sql
-- Exclude very common words
CREATE INDEX docs_tre_idx ON docs USING tre (body)
WHERE length(body) > 20 AND body !~~ '%common_word%';
```

### Error: "regex too complex"

**Fix:** Raise `pg_tre.max_nfa_states`:
```sql
SET pg_tre.max_nfa_states = 50000;
```

**Explanation:** Your pattern compiles to > 10k NFA states. This is rare for normal regexes but can happen with deeply nested alternations or large character classes.

**If you're indexing user input:** This is likely a DoS attempt. Reject the query.

### Index Scan Returns Wrong Rows

**Action:** File a bug report with:
1. `SELECT version();` output
2. Minimal reproduction case (CREATE TABLE + INSERT + query)
3. `EXPLAIN (ANALYZE, BUFFERS, VERBOSE)` output
4. `SELECT tre_extract_debug('your_pattern');` output

**Known causes (fixed in later phases):**
- Phase 5.1 uleven expansion bugs (missing trigram variants)
- Phase 6 selectivity underestimation (planner chooses index when it shouldn't)

### Seq Scan Always Chosen

**Diagnosis:**
```sql
EXPLAIN SELECT * FROM docs WHERE body %~~ tre_pattern('your_pattern');
-- If "Seq Scan" appears, planner thinks it's cheaper
```

**Reasons:**
1. **Non-selective pattern:** `.*foo.*` matches too many rows
2. **Missing statistics:** `ANALYZE docs;` may help
3. **Index not visible:** Check `pg_index.indisready`, `indisvalid`
4. **Cost parameters:** Try `SET random_page_cost = 1.1;` (SSD tuning)

**Force index scan to compare:**
```sql
SET enable_seqscan = off;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM docs WHERE body %~~ tre_pattern('your_pattern');
-- Compare actual cost to seq scan's actual cost
```

### Crash After CREATE INDEX

**Symptom:** `LOG: server process (PID ...) was terminated by signal 11`

**Status:** Known Phase 5 bug (ambuild segfault during bloom population). Fixed in main branch commit `ff69090`.

**Workaround:** Pull latest main, rebuild.

---

## Internals Pointers

For architecture and on-disk format details, see:

- **[doc/design.md](design.md)** — Three-tier funnel, extraction pipeline, recheck flow
- **[doc/onpage_format.md](onpage_format.md)** — Page layouts, WAL records, format versioning
- **[STATUS.md](../STATUS.md)** — Phase-by-phase implementation status

For hacking on pg_tre:
- **src/query/extract.c** — Trigram extraction and tiling
- **src/am/amscan.c** — Three-tier filtering logic
- **src/pages/posting.c** — Posting tree + per-tuple bloom serialization
- **vendor/tre/** — TRE library (submodule)

---

## License

pg_tre is MIT licensed. See [../LICENSE](../LICENSE).

Third-party components:
- TRE: BSD 2-clause (see vendor/tre/LICENSE)
- Lime: Public domain
- sparsemap: MIT

Full attribution in [../NOTICE](../NOTICE).

---

## Contributing

Report issues at: https://codeberg.org/gregburd/pg_tre/issues

When filing bugs:
1. Include `SELECT version();` output
2. Provide minimal reproducer (SQL only)
3. Attach `EXPLAIN (ANALYZE, VERBOSE, BUFFERS)` output
4. Note whether `shared_preload_libraries = 'pg_tre'` is set

Patches welcome via Codeberg PR or email to the author.
