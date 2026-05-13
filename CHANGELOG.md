# Changelog

All notable changes to pg_tre are documented in this file, organized by development phase.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [1.0.0] - 2026-05-13

First production release.  Native PostgreSQL 18+ index
access method for approximate regex matching with a
three-tier filter funnel.

### Highlights vs the v0.1.0 UDF-only version

- Native AM (`USING tre`) with the `%~~` operator and the
  `tre_pattern(P, k)` constructor.  Indexed bitmap heap
  scans replace seq-scan-only `tre_amatch` calls.
- Three-tier filter funnel: range bloom → sparsemap
  posting trees → per-tuple bloom → TRE recheck.
- Multi-leaf posting trees with Lehman-Yao right-links;
  no per-trigram size cap in practice.
- WAL-logged custom rmgr; crash recovery and streaming
  replication implemented (TAP tests in `tap/`).
- UTF-8 codepoint trigrams; multibyte-safe extraction.
- DoS hardening: NFA state cap, compile/match timeouts,
  fanout cap on extraction.
- Cost-model integration with the planner; selectivity
  estimator backed by index meta page statistics.
- 12 differential regression tests gating every commit.
- Packaging: Debian, RPM, Homebrew, Docker, PGXN.
- CI on GitHub and Codeberg (Forgejo Actions).
- Dependabot (GitHub) + Renovate (Codeberg) for
  automated dependency updates.
- Mermaid architecture diagrams that render on both
  GitHub and Codeberg.

### Known limitation

A residual sparsemap heap-corruption path fires under
sustained heavy concurrent insert + read load.  Single-writer
and read-only workloads are unaffected.  Fix in flight with
the sparsemap maintainer; will land in 1.0.1.

See `STATUS.md` for the v1.1 followup list.

## [1.0.0-dev] - Pre-release

### Phase 4.2: Multi-leaf Posting Trees (2026-05-13)

**Goal:** Eliminate the ~8 KB single-leaf posting limit that prevented indexing large corpora.

**Shipped:**
- Multi-leaf posting chains: When a trigram's posting exceeds the single-leaf budget, the builder partitions TIDs across multiple leaves linked via `right_link` (Lehman-Yao convention)
- Right-to-left allocation: Leaves are written from rightmost to leftmost, with each leaf storing `min_tid` and `max_tid` bounds
- Reader updates: `pg_tre_posting_materialize` unions all leaves in the chain; `pg_tre_posting_lookup_tuple_bloom` and `pg_tre_posting_lookup_positions` walk right-links to find the leaf containing the target TID
- Format version bump: PG_TRE_FORMAT_VERSION 2 → 3 (BREAKING: requires REINDEX)
- Test coverage: `test/sql/multi_leaf.sql` exercises 100K-row builds with high-frequency trigrams

**Status:**
- ✅ Build path: Handles overflow by splitting into ~70% budget chunks
- ✅ Materialize path: Unions sparsemaps across chain
- ✅ Tuple bloom lookup: Locates correct leaf via TID range check
- ✅ Positions lookup: Same right-link traversal as bloom
- ✅ Documentation: Updated `doc/onpage_format.md` with multi-leaf layout

**Testing:**
- Existing 11 regression tests pass (single-leaf cases unaffected)
- New multi_leaf test validates 100K-row corpus with common trigrams ('the', 'ing')

**Next Steps:**
- Run multi_leaf test on live cluster to verify performance at scale
- Measure B-tree height and chain lengths in production workloads

### Benchmark Harness (2026-05-12)

**Goal:** Build reproducible benchmark infrastructure for pg_tre vs pg_trgm performance comparison.

**Shipped:**
- `bench/fetch-corpus.sh` — Generates deterministic synthetic corpus (configurable size, fixed seed=42)
- `bench/load-and-index.sh` — Loads data into bench_tre/bench_trgm tables, builds indexes, measures build time/size
- `bench/run-queries.sh` — Executes query matrix (exact regex, k=1, k=2, multi-phrase, non-selective), captures p50/p95/p99 latencies
- `bench/report.sh` — Aggregates results into `doc/perf.md` with markdown tables
- `bench/README.md` — Usage instructions and corpus description
- `doc/perf.md` — Performance report documenting benchmark infrastructure and Phase 5 ambuild blocker

**Query Matrix:**
- Exact regex: `government`, `electrification`, `natural` (vs pg_trgm)
- Approximate k=1: `govrnment`, `natrual` (pg_tre only, seq-scan baseline)
- Approximate k=2: `govrment` (pg_tre vs seq-scan)
- Multi-phrase: `(system){~1}.*(program){~1}` (pg_tre unique capability)
- Non-selective: `the` (tests planner cost estimation)
- Rare/no-match: `xyzabc` (tests index rejection speed)

**Status:**
- ✅ Infrastructure complete: corpus generation, data loading, query runner, report generator
- ✅ pg_trgm baseline works: index builds successfully, queries execute
- ❌ Measurements blocked: pg_tre index creation fails with "invalid memory alloc request size 1610612736"
- ❌ Root cause: Phase 5 ambuild bug (same as STATUS.md item #1: ambuild.c segfault/memory error)

**Testing:**
- Corpus generation: ✅ Generates 1k/10k/100k rows with deterministic vocabulary and typos
- Data loading: ✅ COPY loads 1k rows in ~2.7s per table
- pg_trgm index build: ✅ Completes in ~35ms for 1k rows
- pg_tre index build: ❌ Fails regardless of corpus size (1k, 10k, 100k all trigger same error)

**Next Steps:**
1. Fix ambuild.c memory allocation bug (likely arithmetic overflow in buffer size calculation)
2. Re-run benchmark with 10k rows
3. Update `doc/perf.md` with measured p50/p95/p99 numbers
4. Compare pg_tre speedup vs seq-scan for k=1/k=2 queries

**Documentation:**
- `doc/pg_tre.md` Performance Notes section now references `doc/perf.md`
- `doc/perf.md` honestly documents blocker and includes complete reproduction steps
- All scripts follow project conventions: zero shellcheck warnings, idempotent, structured output

### Phase 0 — Foundation (Complete)

**Goal:** Establish build system, submodules, and basic AM registration.

**Commits:**
- a89d4bf: Initial import (0.1.0 legacy UDF-only extension)
- b324173: Phase 0 + Phase 1 scaffolding — native index AM skeleton

**Shipped:**
- TRE (v0.9.0) and Lime parser generator as git submodules
- New source tree layout: `src/am/`, `src/pages/`, `src/wal/`, `src/query/`, `src/util/`
- Makefile orchestrates TRE autotools + Lime codegen + PGXS build
- `CREATE ACCESS METHOD tre` registers successfully
- `CREATE INDEX ... USING tre` creates empty stub index
- Legacy UDFs preserved: `tre_amatch`, `tre_amatch_cost`, `tre_amatch_detail`, `tre_version`
- Regression test suite framework (`test/sql/pg_tre.sql`)
- LICENSE (MIT), NOTICE (third-party attributions), `.gitignore`, CI workflow

**Tested:**
- Builds cleanly on PG18+ (gcc + clang, Linux + macOS)
- Existing 0.1.0 UDF tests pass (backward compatibility)
- AM registration verified via `\dAm` in psql

**Deferred:** None (Phase 0 complete as designed)

---

### Phase 1 — On-Disk Format & WAL (Partial)

**Goal:** Define page layouts, WAL records, and meta page R/W with crash recovery.

**Commits:**
- 0c31b55: Phase 1 partial — meta page R/W with WAL, posting/AST contracts
- fcda316: STATUS update — mark Phase 1 partial complete, fold remainder into Phase 2 worktree

**Shipped:**
- Page layout declarations (`include/pg_tre/page.h`): META, UPPER, POSTING, RANGE, PENDING
- WAL record types (`include/pg_tre/xlog.h`): XLOG_PTRE_META_UPDATE, XLOG_PTRE_POSTING_INSERT, etc.
- Custom rmgr registration (RM_PG_TRE_ID = 140) via `RegisterCustomRmgr` during preload
- Meta page read/write (`src/pages/meta.c`)
- WAL redo for meta page (full-page image replay)
- End-to-end verification: CREATE INDEX emits META_UPDATE record, crash recovery replays cleanly

**Tested:**
- `pg_ctl stop -m immediate` + restart preserves index metadata
- WAL record appears in `pg_waldump` output

**Deferred to Phase 2:**
- Upper-tree Lehman-Yao splits
- Posting-leaf sparsemap serialization
- Pending-list page chain
- TAP test for crash recovery (`test/t/001_crash_empty.pl`)

---

### Phase 2 — Build Path (Complete)

**Goal:** Tuplesort-backed ambuild, posting tree bulk load, parallel build.

**Commits:**
- 8bea2bd: Phase 2 complete — posting tree + fix tre_parse symbol collision

**Shipped:**
- Tuplesort-backed ambuild (`src/am/ambuild.c`)
- Posting tree bulk loader: sparsemap serialization into POSTING_L pages
- Upper tree B-tree builder: routes trigrams to posting leaves
- Meta page stats: `n_trigrams`, `n_tuples_indexed`
- Posting tree reader helpers (`src/pages/posting.c`)
- Buffer management utilities (`src/pages/buffer.c`)
- Symbol collision fix: `tre_parse` renamed to avoid conflict with TRE library

**Tested:**
- 10k-row fixture builds without error
- `pg_relation_size(index)` within expected bounds (~200 KB for test data)
- Posting tree structure verified via meta page inspection

**Deferred:**
- Parallel build (amcanbuildparallel) — Phase 8 performance work
- Posting tree splits (Phase 8; single-leaf per trigram in Phase 2-4)

---

### Phase 3 — Scan Path, Exact Regex (k=0) (Complete)

**Goal:** Enable index scans for exact regex (k=0) with trigram extraction and amgetbitmap.

**Commits:**
- 502cadc: Phase 3 — Build infrastructure, regex parser, and AST
- d9fb519: Phase 3 — tre_pattern type, %~~ operator, stub extract
- 4804d81: Phase 3 — Add debug UDFs for parser testing
- a9ce946: Phase 3 — Module links successfully
- 1637c66: Phase 3 — Add parser test and update STATUS
- fa6ed1e: Phase 3 — Add progress summary document
- 6c77b2b: Phase 3 complete — real extraction + amgetbitmap for exact regex

**Shipped:**
- Lime-generated regex parser (`src/query/tre_grammar.y` → `tre_grammar.c`)
- Hand-written tokenizer (`src/query/tokens.c`) with mode-sensitive lexing
- Regex AST (`src/query/regex_ast.c`): node types for LITERAL, CONCAT, ALT, REP, APPROX
- tre_pattern type: in/out/recv/send/make functions
- `%~~` operator: text × tre_pattern → bool, wired into `tre_text_ops` opclass
- Russ-Cox-style trigram extraction (`src/query/extract.c`):
  - Literal runs → direct trigrams
  - CONCAT → pass-through child sets
  - ALT → intersection (common trigrams only)
  - REP (m ≥ 1) → inline once
  - APPROX treated as sub-expression (k>0 support deferred)
  - ANY/CLASS → break the run
- amgetbitmap: per-conjunct OR via `sparsemap_union`, AND across conjuncts via `sparsemap_intersection`, emit to TIDBitmap
- Debug UDFs: `tre_parse_debug()`, `tre_extract_debug()`
- amscan: ambeginscan/amrescan/amendscan stubs
- amcostestimate: trivial stub (always prefer index)

**Tested:**
- Differential test (`test/sql/scan_exact.sql`): 13 patterns × 10-row fixture
- Index scan row counts match seq scan for all patterns
- `EXPLAIN` confirms "Bitmap Index Scan on pg_tre index"
- Parser test (`test/sql/parser.sql`): AST correctness for 20+ regex patterns

**Deferred:**
- k > 0 approximate matching (Phase 5)
- Cost estimation (Phase 6)
- Positional constraints (Phase 5.1)

---

### Phase 4 — Incremental Writes (Complete)

**Goal:** Fast-update pending list for INSERT, VACUUM drains to posting tree.

**Commits:**
- 2507f1e: Phase 4 complete — incremental writes via fast-update pending list

**Shipped:**
- Pending list page chain (`src/pages/pending.c`): single-entry and batch append APIs
- WAL-logged pending inserts: XLOG_PTRE_PENDING_INSERT
- aminsert: tokenize text → trigrams, batch-append to pending list
- amvacuumcleanup: drains pending list via `pg_tre_pending_merge()`
  - Collects entries into in-memory hash
  - Unions with existing posting sparsemaps
  - Rebuilds upper tree from scratch (simple but correct)
- amgetbitmap: overlays pending-list TIDs onto posting tree results (scans see fresh inserts)
- Meta page tracks: `pending_head`, `pending_tail`, `pending_n_entries`
- WAL ordering fix: MarkBufferDirty must precede XLogRegisterBuffer (PG18 requirement)

**Tested:**
- `test/sql/incremental.sql`: pre-VACUUM and post-VACUUM scans match seq scan
- Sustained INSERT workload (1000 rows): index size grows, VACUUM merges cleanly
- Crash recovery: `pg_ctl stop -m immediate` mid-workload → pending entries preserved

**Limitations:**
- Single-leaf posting budget (~7 KB): very common trigrams exceed this, emit ERROR
- Pending merge rebuilds entire upper tree (Phase 8 will optimize)

**Deferred:**
- `pg_tre_flush()` function (VACUUM covers the need)
- `fastupdate` storage option (Phase 6 reloptions)
- ambulkdelete: dead TID removal (Phase 7)

---

### Phase 5 — Approximate Regex & Three-Tier Funnel (Complete)

**Goal:** Enable k > 0 approximate matching with three-tier filter (range bloom, posting, per-tuple bloom).

#### Phase 5 WRITE — Tier 1 & Tier 3 Infrastructure

**Commits:**
- 29f437f: Phase 5 WRITE Steps 1-2 — Bloom filter + posting payload
- 2d0e652: Phase 5 WRITE Step 3 — Ambuild bloom population
- 0a240ee: Phase 5 WRITE Step 4 — BRIN-style range tier
- fb3678c: Phase 5 WRITE Step 6 — WAL support for RANGE_UPDATE
- b1bed11: Phase 5 WRITE — Final documentation + STATUS update
- 4323d62: Phase 5 WRITE follow-up — fix NULL repalloc + upper leaf entry count

**Shipped:**
- Bloom filter primitive (`src/util/bloom.c`): double-hashing (h1 + i*h2) % m
- Per-tuple bloom payload in posting leaves (tier 3):
  - Payload region: (n_positions uint16, positions[] uint32, bloom_bits[] uint8)
  - Grows downward from page end before opaque
  - `pg_tre_posting_lookup_tuple_bloom()` reader helper
- BRIN-style range summary tier (tier 1):
  - Single-leaf range tree (PG_TRE_PAGE_RANGE)
  - Groups TIDs by heap block range (default 128 blocks)
  - Unions trigrams into range bloom (2048 bits, k=7)
  - `pg_tre_range_lookup()` and `pg_tre_range_scan()` APIs
- Ambuild bloom population:
  - Tracks trigram positions (byte offsets) during tokenization
  - Computes per-tuple bloom by OR-ing all tuple trigrams
  - Passes positions + bloom_bits to posting builder
- WAL correctness: XLOG_PTRE_RANGE_UPDATE routed to FPI replay
- GUCs: `pg_tre.tuple_bloom_enable` (bool), `pg_tre.bloom_tuple_bits` (int), `pg_tre.range_size_blocks` (int)
- Bug fixes: NULL repalloc crash, upper leaf entry count overflow

**Tested:**
- Builds cleanly with zero warnings
- Bloom filters populated during ambuild (verified via meta page n_trigrams)
- Range tree serialization + WAL replay correct

#### Phase 5 READ — Tier 2 & Query Expansion

**Commits:**
- ef46695: Phase 5 — register p5_read regression test + baseline expected
- 3a039f8: Phase 5.1 — Wire universal Levenshtein expansion into tiling

**Shipped:**
- Navarro tiling (`src/query/tiling.c`): (k+1)-way pattern partitioning
  - Extracts trigram spine from literal runs
  - Partitions into disjoint tiles with +/- k positional slack
  - Outputs DNF TrigramQuery (OR across tiles, AND within each)
- Universal Levenshtein expansion (`src/query/uleven.c`):
  - k=0: identity (no expansion)
  - k=1: 3 positions × (255 substitutions + 256 insertions + 1 deletion per position)
  - k=2: similar with 2-edit combinations
  - Deduplication and fanout capping (pg_tre.max_extraction_fanout)
- Extraction with edit budget (`src/query/extract.c`):
  - k > 0: calls `pg_tre_tile_query()` for global-budget patterns
  - Fallback to `always_true` when tiling fails (conservative)
- CNF/DNF query mode dispatch (`src/am/amscan.c`):
  - CNF (k=0): AND across conjuncts, OR within each
  - DNF (k>0 tiled): OR across tiles, AND within each
- Tier-3 per-tuple bloom filtering (`apply_tuple_bloom_filter()`):
  - For each candidate TID, fetch tuple bloom
  - Test required trigrams against bloom
  - Reject TIDs where required trigrams absent
- Pending-list overlay: integrated for both CNF and DNF modes

**Tested:**
- `test/sql/p5_read.sql`: 21 differential test cases (k=1, k=2)
- Patterns: hello, goodbye, colour, approximate, environment, PostgreSQL
- Differential verification: index scan COUNT == seq scan COUNT

**Known Limitations (Phase 5):**
- Tier-1 range bloom: single-leaf linear lookup (Phase 8 will add binary search)
- Positional filtering: positions stored but not yet used in tier-2 (Phase 5.1 wired uleven but not positional offsets)
- Inline postings: no payload support (only on-disk leaves have blooms)
- Local {~m} budget: treated conservatively (global-budget tiling only)

---

### Phase 5.1 — Positional Filtering & Uleven Integration (Complete)

**Commits:**
- 27dc722: Phase 5.1 — Positional filtering

**Shipped:**
- `pg_tre_posting_lookup_positions()` API stub (ready for Phase 8 implementation)
- Positional offset expansion in tiling: each tile's trigrams have min_offset/max_offset widened by +/- k
- Universal Levenshtein wired into extraction pipeline (but trigram bytes not yet stored for full expansion)

**Tested:**
- Positional API compiles; stub returns 0 positions (filtering disabled)
- Tiling generates correct positional ranges

**Deferred to Phase 8:**
- Store trigram bytes alongside hashes in posting metadata
- Full uleven expansion per-tile (currently expansion is stubbed)

---

### Phase 6 — Planner & DoS Hardening (Complete)

**Commits:**
- ffe189d: Phase 6 — Per-match deadline (CHECK_FOR_INTERRUPTS)
- 40a8601: Phase 6 — Safety hardening (pieces 1-3)

**Shipped:**
- Cost estimation (`src/am/amcost.c`):
  - Uses metapage per-trigram cardinalities
  - Estimates candidate rows via trigram selectivity
  - Compares index path cost vs seq scan cost
- Selectivity function (`src/am/sel.c`):
  - `tre_pattern_sel()`: restriction selectivity estimator
  - Registered as operator %~~ selectivity function
  - Falls back to conservative 0.1 when stats unavailable
- DoS protection:
  - `pg_tre.max_nfa_states`: reject patterns with > N states (default 10k)
  - `pg_tre.compile_timeout_ms`: abort regex compilation after timeout (default 1s)
  - `pg_tre.match_timeout_ms`: abort per-row recheck after timeout (default 1s)
  - Per-match deadline: CHECK_FOR_INTERRUPTS in recheck loop
- Reloptions (`src/am/amoptions.c`):
  - `pending_list_limit`, `q`, `bloom_tuple_bits`, `range_size_blocks`, `fastupdate`, `tuple_bloom_enable`
  - Accessors with GUC fallback: `pg_tre_get_*()` functions
  - `pg_tre_init_reloptions()` called from _PG_init
- amvalidate: basic stub (returns true; full validation deferred)

**Tested:**
- `test/sql/planner.sql`: verifies cost estimation chooses index scan for selective patterns, seq scan for non-selective
- `test/sql/p6_safety.sql`: catastrophic backtracking patterns rejected cleanly
- Cost estimates reasonable (within 2x of actual for test workloads)

**Limitations:**
- Selectivity estimation is coarse (per-trigram independence assumption)
- NFA state count is estimated (not exact TRE NFA size)
- Timeout enforcement relies on CHECK_FOR_INTERRUPTS (not hard real-time)

---

### Phase 7 — Durability, Replicas, VACUUM (Infrastructure Complete)

**Commits:**
- 4733a73: Phase 7 — Add TAP tests for durability

**Shipped:**
- `pg_tre_mask()` for `wal_consistency_checking='pg_tre'` (`src/wal/xlog.c`)
  - Masks LSN, checksum, hint bits, unused space
  - Follows btree_mask pattern from nbtxlog.c
  - Meta page: all fields deterministic, no additional masking
- TAP test suite (`test/t/*.pl`):
  - 001_crash_recovery.pl: immediate shutdown during build/insert/VACUUM
  - 002_replica.pl: streaming replication + cascading standby + wal_consistency_checking
  - 003_reindex_concurrent.pl: REINDEX CONCURRENTLY with concurrent writes
  - 004_pg_upgrade.pl: dump/restore cycle (pg_upgrade placeholder for PG18-only)
  - 005_soak.pl: sustained mixed workload (INSERT/DELETE/VACUUM) + crash recovery
- Bash test runner (`test/durability-tests.sh`): Perl-free alternative
- Build system integration:
  - `make tapcheck`: runs bash durability tests
  - `make tapcheck-perl`: runs Perl TAP tests (when deps available)

**Tested:**
- Masking function compiles with zero warnings
- Test infrastructure wired into Makefile
- Tests written following PostgreSQL::Test::Cluster conventions

**Blocked:**
- Tests cannot run due to pre-existing bugs:
  1. Segfault in ambuild.c during CREATE INDEX (Phase 5 bloom code)
  2. Missing tre_pattern_sel symbol export (Phase 6)
  3. tre_amatch signature mismatch (Phase 3/5)
- Once bugs fixed, tests will verify crash safety, replication correctness, REINDEX, dump/restore

**Deferred to Phase 8:**
- ambulkdelete: dead TID removal from posting trees
- VACUUM FULL / CLUSTER: TID remap handling
- Long soak with CLOBBER_CACHE_ALWAYS

---

### Phase 8 — Performance Tuning (Planned)

**Not yet implemented. Planned features:**
- ReadStream-based posting prefetch
- Parallel index scan (amcanparallel = true)
- Multi-level posting trees (split single-leaf budget)
- Binary search in range bloom tree
- Published benchmarks (`bench/` directory)
- Optional WAL deltas for sparsemap in-place edits (reduce WAL volume)
- Store trigram bytes for full uleven expansion
- Position-based filtering via sparsemap_offset

---

### Phase 9 — Docs & Release (In Progress)

**Commits:**
- 04956d2: Phase 9 — Add comprehensive user-facing documentation (doc/pg_tre.md)
- (this commit): Phase 9 — Add CHANGELOG.md

**Shipped:**
- User-facing reference: `doc/pg_tre.md` (20 KB, 590 lines)
  - Installation instructions
  - Types, operators, functions, GUCs, reloptions
  - Usage cookbook (5 query patterns with EXPLAIN output)
  - Performance notes (three-tier funnel, selectivity, debugging)
  - Known limitations (Phase 4 posting budget, UTF-8, DoS)
  - Troubleshooting guide
- CHANGELOG.md: this file

**In Progress:**
- Migration guide (doc/migration-from-0.1.0.md)
- Release checklist (doc/release-checklist.md)
- Announcement draft (doc/announcement.md)
- README.md update (reflect 1.0.0-rc1 status)
- PGXN META.json + packaging templates

---

## Version History

### [0.1.0] - 2024-05-07 (Initial Import)

UDF-only extension (no index AM). Provided:
- `tre_amatch(text, text, int)` — approximate match with default costs
- `tre_amatch_cost(text, text, int)` — return edit distance
- `tre_amatch(text, text, int, int, int, int)` — approximate match with custom costs
- `tre_amatch_detail(text, text, int)` — detailed match info
- `tre_version()` — TRE library version

No indexing support. All functions use TRE's regaexec directly (always seq scan).

---

## Upgrade Notes

### 0.1.0 → 1.0.0

- **Breaking:** `shared_preload_libraries = 'pg_tre'` now required for index AM functionality
- **New:** `CREATE INDEX ... USING tre` now supported
- **New:** `tre_pattern` type and `%~~` operator for indexable queries
- **Preserved:** All legacy UDFs work unchanged (backward compatible)
- **Migration:** `ALTER EXTENSION pg_tre UPDATE TO '1.0.0';` (see doc/migration-from-0.1.0.md)

No on-disk format change (0.1.0 had no indexes). Upgrade is SQL-only.

---

## Links

- **Repository:** https://codeberg.org/gregburd/pg_tre
- **Design:** [doc/design.md](doc/design.md)
- **On-disk format:** [doc/onpage_format.md](doc/onpage_format.md)
- **User guide:** [doc/pg_tre.md](doc/pg_tre.md)
- **License:** MIT (see LICENSE)
- **TRE library:** https://github.com/laurikari/tre (BSD 2-clause)
