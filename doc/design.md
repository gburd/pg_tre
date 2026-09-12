# pg_tre design

This file is the working design document for the production pg_tre
native index AM.  Paired with `doc/onpage_format.md` (byte-exact page
specs) and the top-level `README.md` (user-facing description).

## 1. Goal

Provide a PostgreSQL index access method, `tre`, that indexes text
columns for fast approximate-regex queries:

    CREATE INDEX docs_tre ON docs USING tre (body);
    SELECT * FROM docs WHERE body %~~ tre_pattern('enviro.{~2}ment', 2);

Delivered via a trigram-posting index over the text column:

1. **Posting tier (authoritative)** -- per-trigram sparsemap
   postings, AND/OR-merged per query to produce a candidate TID set.
   This is the filter that drives the scan.
2. **SuRF prefilter (v10, 3.2.0)** -- a Succinct Range Filter over an
   order-preserving trigram key.  For a `^`-anchored / prefix /
   `LIKE 'foo%'` pattern, the leading trigram maps to a contiguous
   trigram-key range; if the SuRF reports no indexed trigram in that
   range, the scan is rejected before the posting tier or the heap are
   touched.  One-sided error (no false negatives); the heap recheck
   stays authoritative.

A per-tuple bloom tier existed through 2.x and was removed in 3.0.0
(the executor recheck is authoritative, so it added size without
changing results).  A BRIN-style per-block-range bloom tier was built
through 3.1.x but never consulted at scan time; it was removed in 3.2.0.

Recheck is always performed against the heap via TRE's
`regaexec`.

## 2. Prior art

- Russ Cox 2012, Regular Expression Matching with a Trigram Index.
- Navarro & Baeza-Yates 1999, A Hybrid Indexing Method for
  Approximate String Matching.
- Navarro 2001, NR-grep (bit-parallel NFA simulation).
- Myers 1999, bit-parallel Levenshtein.
- Mihov & Schulz 2004, universal Levenshtein automaton.
- Chaudhuri & Kaushik 2009, gram-list intersection with error budget.
- Chambi et al. 2016, Roaring bitmaps (we use the in-house
  sparsemap instead; similar adaptive compression).
- PG Professional RUM: payload-in-posting-tree pattern that we borrow
  for per-tuple blooms and positions.

## 3. Architecture

See the top-level plan in the project history for the complete
architecture; summarized here for ongoing reference.

### 3.1 Layers

    +---------------------------+
    | SQL operator:             |  text %~~ tre_pattern
    | index scan via `tre` AM   |
    +---------------------------+
              |
              v
    +---------------------------+
    | regex AST (Lime-generated)|
    | extract.c  -> tile query  |
    | uleven.c   -> expand near |
    +---------------------------+
              |
              v
    +---------------------------+
    | tier 1 range bloom        |
    | tier 2 posting tree AND/OR|
    | tier 3 per-tuple bloom    |
    +---------------------------+
              |
              v
    +---------------------------+
    | heap recheck (TRE regaexec)|
    +---------------------------+

### 3.2 On-disk

Byte layouts in `doc/onpage_format.md`.  Authoritative definitions
live in `include/pg_tre/page.h`.

Page kinds: META, UPPER, UPPER_L, POSTING, POSTING_L, RANGE,
PENDING.  Each page carries a `PageTreOpaqueData` trailer with the
page kind, flags, and format version.  The meta page (block 0)
carries the format version of the entire index.

### 3.3 Data structure choices

- Posting sets: sparsemap (MIT, in-tree).  The wrap() API lets
  us treat disk bytes as a live sparsemap without copy.
- Bitmaps operations: sparsemap_union / _intersection / _difference
  for AND-of-OR query evaluation.
- Positional filtering: sparsemap_offset for +/-k shifts.
- Blooms: custom 128/2048-bit bloom filters with pg_prng-backed
  double-hashing; one implementation used at both tuple and range
  scales, different m/k parameters.
- Parser: Lime LALR(1) (public domain); grammar in
  `src/query/tre_grammar.y`.  Tokenizer is hand-written
  (`src/query/tokens.c`) because regex is mode-sensitive.
- Match recheck: TRE 0.9.0 (BSD, statically linked from
  vendor/tre).

### 3.4 Trigram extraction (Phase 3.5: codepoint-based)

**Codepoint trigrams (introduced in format v2; current on-disk
format is v5):** pg_tre indexes text using **Unicode
codepoint trigrams**, not byte trigrams.

Each trigram is a sequence of 3 consecutive Unicode codepoints (int32
values in the range 0x0000..0x10FFFF), not 3 bytes. For pure ASCII text,
this is equivalent to byte trigrams (each byte IS a codepoint), so no
regression on ASCII-only workloads.

For multi-byte UTF-8:
- A 3-byte CJK character like '東' is a single codepoint (U+6771).
- The trigram '東京タ' is hash(0x6771, 0x4eac, 0x30bf), not a hash of 9 bytes.
- Query patterns and indexed values decompose identically, so matches are found.

**Motivation:** Byte trigrams fail for multi-byte UTF-8 because trigram
boundaries don't align with character boundaries. A query pattern '東京'
decomposes into different byte trigrams than the indexed text '東京' when
concatenated with neighbors, causing false negatives.

**Implementation:**
- `src/util/utf8.c`: streaming UTF-8 decoder (`PgTreCpStream`).
- `src/util/hash.c`: `pg_tre_hash_trigram_cp(const int32 cp[3])` for codepoint trigrams.
- `src/am/ambuild.c`, `src/am/aminsert.c`: replaced byte-walk loops with codepoint streaming.
- `src/query/extract.c`: regex AST extraction uses codepoint runs, not byte runs.

**Migration:** Indexes built with v1 (byte trigrams) must be REINDEXed after
upgrade. The meta page format version is bumped from 1 to 2. Old indexes
open cleanly but will not match UTF-8 queries correctly.

### 3.5 WAL

**As of v4.0.0:** Uses PostgreSQL's generic WAL facility (`access/generic_xlog.h`). No custom rmgr, no `shared_preload_libraries` required. 

**Historical:** v3.x used custom rmgr RM_PG_TRE_ID (140), which required preload. Removed in 4.0.0 for simpler deployment.

## 4. Phase table

    Phase 0  foundation               -- shipped (this commit)
    Phase 1  on-disk format + WAL     -- in progress
    Phase 2  build path               -- planned
    Phase 3  scan path (k=0)          -- planned
    Phase 4  incremental writes       -- planned
    Phase 5  approximate + 3-tier     -- planned
    Phase 6  planner + DoS            -- planned
    Phase 7  durability / replicas    -- planned
    Phase 8  performance tuning       -- planned
    Phase 9  docs & 1.0.0 release     -- planned

See `STATUS.md` for the live phase state.

## 5. Non-goals

- Full-text ranking.  Use RUM or pg_trgm if you need BM25-style
  scoring.
- Vector similarity search.  Use pgvector.
- Index-only scans.  The recheck is mandatory.
- Equality / range queries.  Use btree.

## 6. Open questions

- Per-tuple bloom width: 128 default, experiment in Phase 5.
- Range size: 128 blocks default, experiment in Phase 5.
- Should positional payload be opt-in (`WITH (positional=off)`) to
  save space when positions don't help queries?  Decide in Phase 6
  once real benchmarks exist.
