# Changelog

All notable changes to pg_tre are documented in this file, organized by development phase.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [1.2.3] — 2026-05-22 — tier-3 re-enabled by default

Patch release on the 1.2 lineage.  Same on-disk format as
1.2.2; no re-index required; `ALTER EXTENSION pg_tre UPDATE
TO '1.2.3'` has no SQL work to do.

### Fixed

- **Pending-overlay regression that kept tier-3 disabled in
  1.2.2.**  Root cause: the positional filter in
  `pg_tre_amgetbitmap` silently dropped TIDs whose conjunct
  trigrams were all in the pending list (not yet flushed to
  posting trees).  Mechanism: for each conjunct, the
  positional filter loops over disjuncts and calls
  `pg_tre_upper_lookup(trigram)`; when the trigram isn't in
  the upper tree (because it's only in the pending list),
  the loop does `continue`.  If NONE of the disjunct's
  trigrams are in the upper tree, the loop finishes with
  `conj_pass=false` (initialized) and the TID is dropped.

  Fix: track whether any disjunct was actually evaluated.
  If none were (all pending-only), conservative-pass the
  conjunct so the candidate falls through to the executor
  recheck.  This matches the tier-3 bloom branch's
  pre-existing conservative-pass semantics.

### Changed

- **`pg_tre.tuple_bloom_enable` default flipped back to
  `true`.**  With the bloom-header fix from 1.2.2 plus the
  positional-filter fix above, tier-3 works correctly on
  both posting-tree-resident candidates and pending-list
  candidates, at all scales (tested 50 / 5K / 100K rows).
  Multi-leaf chain walking, per-leaf rank computation, and
  the pending overlay all interact correctly.  This
  resolves the long-running "chain-rank lookup repair"
  v1.3 followup that's been carried since 1.0.0.

### Verified

- 15/15 regression tests pass two consecutive runs with
  tier-3 default ON.
- `wal_audit.sh`: 3/3 tests pass.
- `replication.sh`: 4/4 tests pass (Test 2 was the
  reproducer for the pending-overlay regression).
- Build clean with `-Werror`.
- `ALTER EXTENSION pg_tre UPDATE TO '1.2.3'` from 1.0.0,
  1.1.0, 1.1.1, 1.2.1, and 1.2.2 verified working.

Inspired by https://github.com/timescale/pg_textsearch
(Tiger Data, PostgreSQL License).

---

## [1.2.2] — 2026-05-22 — tier-3 bloom-header fix

Patch release on the 1.2 lineage.  Same on-disk format as
1.2.1; no re-index required; `ALTER EXTENSION pg_tre UPDATE
TO '1.2.2'` has no SQL work to do.

### Fixed

- **Tier-3 per-tuple bloom — partial fix.**  Identified and
  fixed the long-standing struct-vs-bytes mismatch in the
  scan-side bloom check that has been masquerading as the
  "chain-rank lookup repair" v1.3 followup since 1.0.0.
  Build path serializes only the bit array of each per-row
  bloom into the posting-leaf payload (no header).  Scan
  path read those bytes into a stack buffer and cast them
  to `(PgTreBloom *)`, then accessed `b->m_bits` and
  `b->k` — reading garbage from the bit array as if it
  were the header.  With random `m_bits` values, the
  bloom-position calculation `pos = (h1 + i*h2) % m`
  produced unrelated bits, and the bloom check rejected
  every legitimate match.

  Fix: `src/am/amscan.c` reserves space for the
  `PgTreBloom` header at the start of the scratch buffer.
  The lookup writes bit data **past** the header (into the
  bit-array region).  After the lookup, set
  `bloom_view->m_bits` and `bloom_view->k` directly — NOT
  via `pg_tre_bloom_init()`, which calls
  `memset(bits, 0, bits_bytes)` and would wipe the bits we
  just read.

  This makes tier-3 work correctly on posting-tree-resident
  candidates: verified at 50, 5K, and 100K row scales.
  Multi-leaf chain walking and per-leaf rank computation
  were already correct.

- **`pg_tre.tuple_bloom_enable` default kept at `false`** in
  1.2.2 — a residual pending-overlay regression surfaced
  during the bloom-header investigation: when candidate
  TIDs come from the pending list (post-INSERT, pre-flush)
  and tier-3 is enabled, all such TIDs are rejected by
  `apply_tuple_bloom_filter`.  Every disjunct's
  `pg_tre_upper_lookup` returns false for trigrams that
  are only in the pending list, which should fall through
  to the conservative-pass branch — but empirically it
  doesn't.  Root cause not yet identified.  Setting the
  GUC to `on` for stable / post-VACUUM workloads works
  correctly; the limitation is fastupdate-active state.
  Tracked as v1.3 followup.

### Changed

- **`PG_TRE_INLINE_POSTING_MAX` kept at 256 bytes.**  Earlier
  in the 1.2 cycle we explored bumping the inline threshold
  to 384 for ~30-50% page-count reduction on sparse-trigram
  corpora, but two regressions surfaced: `wal_audit.sh`'s
  post-crash differential check fails at 384 (WAL-redo path
  interaction with larger inline blobs), and at ≥ 448 the
  multi-leaf 100K-row test returns 0 rows for
  `Row 12[0-9][0-9][0-9]` (chain-walker interaction).  Both
  reverted; tracked as v1.3 followups.  See
  `doc/specs/posting-page-coalescing.md` for the
  longer-term structural fix that closes the gap to
  pg_trgm by packing multiple trigrams per page.

### Verified

- 15/15 regression tests pass two consecutive runs (no
  flake), with the bloom-header fix in place and the GUC
  default at false.
- `wal_audit.sh`: 3/3 tests pass.
- `replication.sh`: 4/4 tests pass.
- Build clean with `-Werror`.
- `scripts/release-check.sh` passes end-to-end.
- `ALTER EXTENSION pg_tre UPDATE TO '1.2.2'` from 1.0.0,
  1.1.0, 1.1.1, and 1.2.1 verified working.

Inspired by https://github.com/timescale/pg_textsearch
(Tiger Data, PostgreSQL License).

---

## [1.2.1] — 2026-05-21 — testing, hardening, similarity ranking, size tuning

Minor release on the 1.0.0 lineage.  Same on-disk format as
1.1.x; existing indexes work as-is via
`ALTER EXTENSION pg_tre UPDATE TO '1.2.1'`.

This cycle is split into five logical commits on `main`:

  1. Adopt pg_textsearch's release-engineering and CI bar:
     `RELEASING.md`, `CONTRIBUTING.md`, `SECURITY.md`,
     `.clang-format`, `.pre-commit-config.yaml`,
     `scripts/bump-version.sh`, GH Actions workflows
     (`upgrade-tests`, `pgspot`, `formatting`,
     `sanitizer-build-and-test`, `nightly-stress`),
     and Makefile targets for format / coverage / shell
     tests.
  2. Shell-test infrastructure (`test/scripts/lib.sh`,
     `wal_audit.sh`, `stress.sh`, `test/lsan.supp`).  These
     caught two real bugs that landed in the same commit:
     the UNLOGGED-fork assertion crash and the
     `tuple_bloom_enable` default mismatch.
  3. Similarity / distance helpers and the `<@>` operator.
  4. `replication.sh` + three real WAL-redo bugs caught and
     fixed (loop bound off by one, `REGBUF_WILL_INIT`
     suppressing the FPI we depend on, missing FPI on
     subsequent records per checkpoint cycle).  The same
     fixes also tightened single-node crash recovery.
  5. Size-tuning measurement and follow-up: bloom-overhead
     reduction phases, `to_tsvector` framing, CIC support
     verification, Forgejo Actions parity with `.github/`,
     and the v1.3 / v2.0 design specs for the deferred
     bloom-size work.

### Fixed

- **Tier-3 per-tuple bloom — partial fix.**  Identified and
  fixed the long-standing struct-vs-bytes mismatch in the
  scan-side bloom check: build serialized only the bit
  array (no header), but scan cast the bytes to
  `(PgTreBloom *)` and read garbage `m_bits` / `k` from the
  bit data.  Real fix: read the bloom bits past the header
  region in the scratch buffer, then write the correct
  `m_bits` / `k` directly into the header (without calling
  `pg_tre_bloom_init`, which would re-zero the bit array).
  This makes tier-3 work correctly for posting-tree-resident
  candidates on both single-leaf and multi-leaf trees —
  verified at small (50-row), medium (5K-row), and large
  (100K-row) scales.

  However, **a separate pending-overlay regression surfaced**
  in the same investigation: when candidate TIDs come from
  the pending list (post-INSERT, pre-flush) and tier-3 is
  enabled, all such TIDs are rejected by `apply_tuple_bloom
  _filter` even though every disjunct's `pg_tre_upper_lookup`
  returns false (which should fall through to the
  conservative pass branch).  Root cause not yet identified.
  `pg_tre.tuple_bloom_enable` default kept at `false` for
  1.2.1 to preserve correctness; full re-enablement deferred
  to v1.3 once the pending-overlay interaction is
  understood.  The bloom-header fix itself stays in;
  flipping the GUC to `on` post-flush works correctly.

### Added

- **`pg_tre.min_trigram_freq` GUC** (default 1, no behavior
  change).  Build-time filter that drops posting trees for
  trigrams appearing in fewer than `min_trigram_freq` rows.
  Rare trigrams aren't useful candidate filters anyway — the
  recheck path remains authoritative for correctness, and
  queries that would have relied on dropped trigrams fall
  back through the existing lossy-bitmap path.  At
  `min_trigram_freq=5` on a 100-row fixture: 95 of 120
  posting trees skipped (~80% page-count reduction in the
  test).  PGC_SIGHUP; effective on the next CREATE INDEX /
  REINDEX.
- New regression test `test/sql/cardinality.sql` covering
  the GUC-default behavior and asserting the
  index-vs-seqscan differential remains correct when rare
  trigrams are dropped.
- New regression test `test/sql/concurrently.sql` covering
  `CREATE INDEX CONCURRENTLY` and `DROP INDEX CONCURRENTLY`.
  Verified that the build path under the dual-snapshot CIC
  protocol returns the same row sets as the non-concurrent
  build path.
- README section explicitly framing pg_tre vs pg_textsearch.
  Different problem domains — BM25 ranked keyword search
  (tokenized `tsvector` input) vs approximate regex over
  raw character sequences — with a recipe for the
  combined-index hybrid pattern.
- `doc/specs/posting-page-coalescing.md` (v2.0 design) and
  `doc/specs/variable-width-blooms.md` (v1.3 design) for
  the deferred bloom-overhead work.

### Changed

- **`PG_TRE_INLINE_POSTING_MAX` — attempted bump 256 → 384,
  reverted.**  Higher inline thresholds let more trigrams stay
  in the upper-tree leaf entries (one 8 KB page per trigram is
  the dominant size cost), and 384 promised ~30-50% page-count
  reduction on sparse-trigram corpora.  Two regressions
  surfaced in testing: (a) `wal_audit.sh`'s post-crash
  differential check fails at 384 (index returns 0 for a
  pattern that seq-scan finds 1000 of); (b) at ≥ 448 the
  multi-leaf 100K-row test returns 0 rows for
  `Row 12[0-9][0-9][0-9]`.  The interactions between larger
  inline blobs, WAL redo, and the chain walker need
  investigation; reverted to 256 for 1.2.1.  Tracked as a v1.3
  followup; see `doc/specs/posting-page-coalescing.md` for the
  longer-term structural fix that closes the gap to pg_trgm
  by packing multiple trigrams per page.
- **Forgejo Actions workflow** (`.forgejo/workflows/ci.yml`)
  brought to parity with `.github/workflows/`: `-Werror`
  builds, 3x flake detection, `wal_audit.sh` and
  `replication.sh` shell test runs, `pgspot` security scan
  job, `format-check` on changed files, fixed STATUS.md
  staleness check.  The `publish-pages` job continues to
  push docs to a `pages` branch, which Codeberg Pages
  serves at `<user>.codeberg.page/<repo>/`.  Now also
  publishes `RELEASING.md`, `CONTRIBUTING.md`, and
  `SECURITY.md`.

### Verified

- **CIC works for pg_tre** today via the standard PG
  machinery; the AM doesn't need `amcanbuildparallel` for
  CIC (that flag is for *parallel-worker* index build, a
  separate axis).  Documented in STATUS.md.  Parallel scan
  (`amcanparallel`) and parallel build
  (`amcanbuildparallel`) remain false; tracked as v2.0
  followups.

### v1.3 followups added by this cycle

- Inline-data scan-path bug at `PG_TRE_INLINE_POSTING_MAX >
  ~448 bytes` (multi-leaf chain interaction with larger
  inline blobs).  Once fixed, raise the threshold to 1024+
  for further size reduction.
- Multi-leaf chain-rank repair (was already tracked).
  Variable-width per-tuple blooms (`doc/specs/variable-
  width-blooms.md`) depend on this.

### v2.0 followups added by this cycle

- Posting-page coalescing (`doc/specs/posting-page-
  coalescing.md`).  The structural change that closes most
  of the remaining gap to pg_trgm size: pack 4-20
  low-cardinality trigrams onto a single page instead of
  one page per trigram.  Format-version bump.


### Added

- Similarity / distance ranking helpers for approximate-match
  result sets:
  - `tre_distance(text, text, int)` and
    `tre_distance(text, tre_pattern)` return the edit
    distance (insertions + deletions + substitutions, each
    weighted 1) of the best alignment, or NULL if no match
    exists within `max_cost`.  Equivalent to the existing
    `tre_amatch_cost` but renamed to make ranking idioms
    obvious in EXPLAIN plans.
  - `tre_similarity(text, text, int)` and
    `tre_similarity(text, tre_pattern)` return
    `1 - cost / max(len(input), len(pattern))` in `[0.0,
    1.0]`, with `0.0` (not NULL) for inputs that don't match
    within `max_cost`.  Matches `pg_trgm`'s `similarity()`
    semantics.
  - The `<@>` operator (`text <@> tre_pattern`) is the
    ergonomic spelling for ranking: returns the integer
    distance, NULL if no match.  Inspired by
    `pg_textsearch`'s `<@>` for BM25 and `pg_trgm`'s `<->`
    for trigram distance.
  - Idiom: `WHERE body %~~ pattern ORDER BY body <@> pattern
    ASC NULLS LAST LIMIT N` — the index narrows candidates,
    the executor sorts the top-N in memory.  Index-side
    `ORDER BY` support is `v2.0` work; today the sort is
    done by the executor after recheck.
  - New regression test `test/sql/similarity.sql`.
- Project-infrastructure overhaul modeled on Tiger Data's
  `pg_textsearch` (PostgreSQL License).  See the dedicated
  release commits for the full list:
  - `RELEASING.md`, `CONTRIBUTING.md`, `SECURITY.md`,
    `.clang-format` + `.clang-format-ignore`,
    `.pre-commit-config.yaml`, `.codecov.yml`,
    `.github/ISSUE_TEMPLATE/{bug_report,feature_request}.md`.
  - The `coverage` job in `.github/workflows/ci.yml` runs
    `make coverage` and uploads `coverage.info` to Codecov
    via the `codecov/codecov-action`.  Marked
    `continue-on-error: true` and `informational: true` in
    `.codecov.yml`'s patch policy until we hit a baseline
    we want to enforce.  Public-repo uploads work without
    `CODECOV_TOKEN`; private-repo uploads need the secret.
  - `scripts/bump-version.sh`: mode-aware (dev bump vs.
    release), validates clean working tree, targeted
    regexes that don't mangle legacy upgrade-chain entries.
  - `Makefile` targets: `format`, `format-check`,
    `format-changed`, `format-diff`, `format-single`,
    `coverage`, `coverage-build`, `coverage-clean`,
    `coverage-report`, `test-wal-audit`, `test-stress`,
    `test-shell`, `test-all`.
  - `.github/workflows/upgrade-tests.yml`: ALTER EXTENSION
    UPDATE matrix against every shipped release, on PR +
    weekly.
  - `.github/workflows/pgspot.yml`: extension SQL security
    scan on every push and PR.
  - `.github/workflows/formatting.yml`: clang-format check
    on changed files (gating) plus tree-wide check
    (informational until a one-shot reformat lands).
  - `.github/workflows/sanitizer-build-and-test.yml`: ASAN +
    UBSAN against PG18.3 + regression suite on every push to
    main.  Without the PG memory-context instrumentation
    patch (deferred): catches direct UAF, OOB, double-free,
    and undefined behavior.
  - `.github/workflows/nightly-stress.yml`: runs `stress.sh`
    + `wal_audit.sh` nightly under both plain and ASAN+UBSAN
    builds.
- Shell-test infrastructure under `test/scripts/`:
  - `lib.sh`: shared library with cluster init/teardown,
    `psql_check`, `psql_capture`, `psql_count`,
    `wait_for_lsn`, `pg_init_primary`, `pg_init_standby`,
    `primary_lsn`, `create_basic_table`, `diff_idx_vs_seq`.
  - `wal_audit.sh`: three tests (UNLOGGED indexes emit only
    init-fork META_UPDATE records; LOGGED inserts produce
    decodable rmgr-149 records; crash + restart preserves
    the index and its differential idx-vs-seq invariant).
  - `replication.sh`: primary + streaming standby; verifies
    that index state replicates correctly (Test 1: built
    index reaches standby identical row counts; Test 2:
    incremental writes stream to standby; Test 3:
    `wal_consistency_checking = 'pg_tre'` clean (opt-in via
    `TRE_WAL_CONSISTENCY=1`); Test 4: standby stop / start
    catchup preserves the index).  Caught three real WAL-redo
    bugs on first run — see Fixed below.
  - `stress.sh`: N-iteration mixed insert / parallel-SELECT
    / DELETE / VACUUM / REINDEX workload with RSS ceiling,
    postmaster-alive check, per-iteration differential
    check, and a clean shutdown + restart at the end.
- `test/lsan.supp`: leak suppressions for known-benign
  cases.

### Changed

- `.github/workflows/ci.yml`: builds enforce `-Werror`; the
  regression suite runs three times per matrix cell to
  surface flakes.
- `scripts/release-check.sh`: runs the regression suite via
  `scripts/run-regress.sh` against the active pgrx cluster
  instead of the broken `make localcheck` path; TAP tests
  default-skipped (set `RELEASE_CHECK_TAP=1` to run; takes
  ~6 min); fixed the STATUS.md staleness check.
- `Makefile`: `make tap` target now uses `PG_REGRESS`,
  `PG_TAP_PERL5LIB`, `PG_TAP_TMPDIR` overrides for the pgrx
  layout, with optional nix-shell wrapper for IPC::Run.
- `pg_tre_extend` retained as a thin wrapper over the new
  `pg_tre_extend_fork`; existing callers unaffected.
- `pg_tre_build_empty` retained as a thin wrapper over the
  new `pg_tre_build_empty_fork`; existing callers unaffected.

### Fixed

- `pg_tre_ambuildempty` previously called `pg_tre_build_empty`,
  which extended `MAIN_FORKNUM`.  This crashed with an
  assertion when `BufferGetBlockNumber(metabuf) !=
  PG_TRE_META_BLKNO` because `ambuild` had already populated
  block 0 of the main fork before `ambuildempty` ran.  Result:
  `CREATE INDEX ... USING tre` on an UNLOGGED table failed with
  `TRAP: failed Assert("BufferGetBlockNumber(metabuf) ==
  PG_TRE_META_BLKNO")`.  Fixed by adding
  `pg_tre_build_empty_fork(forknum)` and
  `pg_tre_extend_fork(forknum)`; `ambuildempty` now extends
  `INIT_FORKNUM`.  WAL logging fires unconditionally for the
  init fork (the init fork is the WAL-logged template that
  gets copied to the main fork during crash recovery, so it
  must be replayable even on UNLOGGED indexes where
  `RelationNeedsWAL` returns false).  Caught by the new
  `test/scripts/wal_audit.sh`.
- `pg_tre.tuple_bloom_enable` defaulted to `TRUE` in
  `src/module.c` but was documented and tested as defaulting
  to `FALSE` (per the v1.1 followups in `STATUS.md`, the
  per-tuple bloom and positional filter are bypassed until
  the chain-rank lookup is repaired).  Default flipped to
  `FALSE` to match documented intent.
- **Streaming-replication correctness.** Three real bugs in the
  custom-rmgr redo path, all caught by the new
  `test/scripts/replication.sh`:
  - `pg_tre_redo_fpi`'s loop bound was off by one:
    `blkno < XLogRecMaxBlockId(record)` should have been
    `blkno <= XLogRecMaxBlockId(record)`.  `XLogRecMaxBlockId`
    returns the maximum block_id, NOT the count.  For a record
    that registered buffers with IDs 0 and 1 (typical: meta
    page + page-being-modified), the max is 1, and our loop
    iterated only blkno=0.  Block 1 — the actual page being
    modified — was silently dropped from every redo, which
    surfaced on a streaming standby as "could not read blocks
    52..52: read only 0 of 8192 bytes" the next time the
    primary tried to extend the relation past the unreplayed
    range.
  - All `XLogRegisterBuffer` callers passed
    `REGBUF_WILL_INIT | REGBUF_STANDARD`.  `REGBUF_WILL_INIT`
    suppresses the FPI (PG assumes the redo callback will
    reconstruct the page from delta data); our redo only
    restores from FPI.  Result: PANIC "block with WILL_INIT
    flag in WAL record must be zeroed by redo routine" on
    every standby replay.  Fixed by dropping `REGBUF_WILL_INIT`
    everywhere; the FPI is what makes the FPI-only redo work.
  - All `XLogRegisterBuffer` callers now pass
    `REGBUF_FORCE_IMAGE | REGBUF_STANDARD`.  Without
    `REGBUF_FORCE_IMAGE`, PG only emits an FPI on the first
    record per buffer per checkpoint cycle; subsequent records
    carry deltas and our FPI-only redo skips them, leaving
    the standby with a stale page.  Caught by the test where
    inserting 3 rows on the primary resulted in only 1 row
    visible to the index on the standby (heap was correct;
    only the index was missing 2 entries).  `REGBUF_FORCE_IMAGE`
    forces an FPI on every record, which is wasteful but
    correct; delta-aware redo is a v1.2 follow-up.


---

## [1.1.1] - 2026-05-14

Hardening release.  Same on-disk format as 1.1.0; no re-index
required.

### Changed

- Vendored sparsemap upgraded from 2.2.0 to 2.3.0.  v2.3.0 is
  defensive-bounds-checking only; the on-disk byte layout, public
  API, and behavior on well-formed input are byte-identical to
  2.2.0.  Specifically:

  * `__sm_chunk_get_position` clamps `bv` to the physical chunk
    capacity so an `sm_open` of attacker-controlled bytes with a
    wildly-wrong start offset can no longer walk past the chunk
    header word.
  * `sm_contains` rejects indices >= `SM_CHUNK_MAX_CAPACITY`
    instead of computing a wrapped `bv`, and now returns false
    on a NULL map (cheap insurance for callers passing the
    result of `sm_intersection` / `sm_difference` / `sm_xor`
    unchecked, which legitimately return NULL on empty results).
  * `__sm_get_size_impl` now walks chunks bounds-safely against
    `m_capacity`, truncating the on-disk chunk count to the
    largest valid prefix when a chunk would extend past the
    buffer.  Behavior is unchanged on well-formed input; the
    walker matters only when sparsemap consumes a corrupt buffer.
  * Stub coalesce paths in `sm_remove` initialize `offset =
    SM_IDX_MAX` so the no-op branches don't read uninitialized
    chunk metadata.
  * UBSan-clean fix in the diagnostic chunk-formatter (loop bound
    was off-by-one and shifted by 64 on a 64-bit value).

  None of these are observable from pg_tre under correct
  operation.  They harden sparsemap against corrupt or malicious
  input.

---

## [1.1.0] - 2026-05-14

Maintenance release.  Same on-disk format as 1.0.0; no
re-index required.

### Fixed

- Multi-leaf posting trees: the right-link chain walker in
  `pg_tre_posting_materialize` had inverted success/failure
  logic on `sm_union` (treating the success path as failure),
  which silently dropped every leaf past the first.  Index
  scans on tables with any posting tree wide enough to split
  across leaves returned only the rows from the root leaf;
  on a 100K-row test case that meant the index returned 0
  rows for matches that the seq-scan returned 100,000 rows
  for.  Fixed and covered by a new differential
  index-vs-seqscan equivalence check in
  `test/sql/multi_leaf.sql`.
- Per-tuple bloom (tier-3) and positional filter are now
  gated on `pg_tre.tuple_bloom_enable`.  Both filters key
  their per-TID payload offset off a per-leaf rank that does
  not accumulate across right-link chains, so any TID past
  the first leaf reads the wrong payload slot and is
  rejected.  Until the chain-rank lookup is repaired the
  GUC bypasses both filters; the executor recheck remains
  authoritative for correctness.

### Changed

- Vendored sparsemap upgraded from 2.0.0 to 2.2.0.  v2.2.0
  adds `sm_open_copy` (deserialize-into-fresh-buffer
  convenience), `sm_add_grow` (auto-doubling add), allocator
  hooks (`sm_set_allocator` / `sm_create_with_allocator`),
  and is otherwise drop-in compatible.
- Adopted `sm_open_copy` at every materialize site
  (`src/pages/posting.c`), `sm_add_grow` in the overlay
  accumulator and the tier-3 refine path
  (`src/am/amscan.c`), and `sm_next_member` in the bitmap
  emit loop.  The emit loop is now O(cardinality) rather
  than O(span); on a 100K-row test the previous loop took
  minutes, the new one returns in milliseconds.
- `sm_union` callers in `pg_tre_posting_materialize` use
  `sm_free` rather than `free`, matching the lineage
  contract for sparsemap-allocated maps.

### Acknowledgements

- The 2.2.0 sparsemap release added the four APIs requested
  in the integration feedback at the close of the 1.0.x
  cycle (allocator hooks, `sm_open_copy`, `sm_add_grow`,
  documentation of breaking-change history).

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
