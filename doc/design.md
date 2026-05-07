# pg_tre design

This file is the working design document for the production pg_tre
native index AM.  Paired with `doc/onpage_format.md` (byte-exact page
specs) and the top-level `README.md` (user-facing description).

## 1. Goal

Provide a PostgreSQL index access method, `tre`, that indexes text
columns for fast approximate-regex queries:

    CREATE INDEX docs_tre ON docs USING tre (body);
    SELECT * FROM docs WHERE body %~~ tre_pattern('enviro.{~2}ment', 2);

Delivered via a three-tier filter funnel over trigram postings:

1. **Range tier** -- BRIN-style block-range blooms reject whole
   heap regions that lack any required trigram.
2. **Posting tier** -- per-trigram sparsemap postings, AND/OR-merged
   to produce a candidate TID set.
3. **Per-tuple tier** -- 128-bit bloom per indexed tuple stored
   inline with the posting, used to refine the candidate set
   without heap I/O.

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

### 3.4 WAL

Custom rmgr RM_PG_TRE_ID (140 by default).  Record types declared in
`include/pg_tre/xlog.h`.  Registered only when the extension is
loaded via `shared_preload_libraries`; the legacy UDFs work
regardless.

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
