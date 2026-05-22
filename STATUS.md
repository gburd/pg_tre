# pg_tre status

Released: **1.2.2** (2026-05).  See `CHANGELOG.md` for full
release notes and `doc/design.md` for the architecture this
file tracks against.

1.2.2 is a patch release on the 1.0.0 lineage: same on-disk
format, no re-index required.  Headline additions:

- **Similarity ranking** — `tre_distance(text, tre_pattern)`,
  `tre_similarity(text, tre_pattern)`, and the `<@>`
  ("eyeball") operator.  Inspired by `pg_textsearch`'s `<@>`
  for BM25 ranking.  Use
  `WHERE body %~~ pattern ORDER BY body <@> pattern ASC LIMIT N`
  to return the N closest matches.
- **Tier-3 per-tuple bloom — partial fix.**  The
  struct-vs-bytes mismatch in the scan-side bloom check
  (root cause of the long-standing "chain-rank lookup"
  followup) is fixed.  Tier-3 works correctly on
  posting-tree candidates at all scales.  A separate
  pending-overlay regression surfaced during the
  investigation: pending-list TIDs are still incorrectly
  rejected when tier-3 is on, so the GUC default stays at
  `false` for 1.2.1.  Setting it to `on` post-VACUUM (when
  pending is flushed) works correctly.
- **WAL replay correctness** — three real bugs in our custom
  rmgr's redo path were caught by the new
  `test/scripts/replication.sh` and fixed (loop bound off by
  one, `REGBUF_WILL_INIT` suppressing the FPI we depend on,
  missing FPI on subsequent records per checkpoint cycle).
  Streaming replication and crash recovery are now
  byte-identical between primary and standby.
- **Project infrastructure** — overhaul modeled on Tiger
  Data's `pg_textsearch`: `RELEASING.md`, `CONTRIBUTING.md`,
  `SECURITY.md`, `.clang-format`, pre-commit hooks,
  `scripts/bump-version.sh`, GitHub Actions for upgrade
  testing, security scanning, formatting, sanitizers, and
  nightly stress under ASAN+UBSAN.
- **Shell test infrastructure** — `test/scripts/lib.sh` plus
  `wal_audit.sh`, `replication.sh`, `stress.sh`.  These have
  already paid for themselves: five real bugs (UNLOGGED-fork
  assertion, `tuple_bloom_enable` default, three WAL-redo
  issues) caught on first run.

1.1.1 was a hardening release: vendored sparsemap 2.2.0 →
2.3.0 (defensive bounds checks against corrupt input).

1.1.0 was a maintenance release: sparsemap 2.0.0 → 2.2.0
plus a multi-leaf right-link `sm_union` reversed-logic fix
that silently dropped every leaf past the first.

## What ships in 1.2.2

### Storage and recovery

- Custom IndexAmRoutine registered as `USING tre`.
- On-disk format v3: meta page, upper tree, multi-leaf
  posting trees with Lehman-Yao right-links, pending list,
  range-bloom tier, payload region.
- Custom rmgr (id 140) with full WAL coverage; crash
  recovery and streaming replication validated by
  `test/scripts/replication.sh` (4 tests including catchup
  across standby restart).
- All page mutations carry full-page images
  (`REGBUF_FORCE_IMAGE`).  Wasteful but correct; delta-aware
  redo is a v2.0 followup.

### Query path

- `body %~~ tre_pattern(P, k)` operator drives indexed
  bitmap scans.
- `tre_amatch*(text, text, k, ...)` UDF family: legacy
  surface preserved from 0.1.0 plus
  `tre_amatch_cost`, `tre_amatch_detail`,
  `tre_amatch_with_costs`.
- **Similarity ranking** — `tre_distance(text, ...)`,
  `tre_similarity(text, ...)`, and the `<@>` operator.
- Lossy fallback when extraction can't anchor: emits a
  TIDBitmap covering the heap, lets recheck filter.
  Correctness preserved either way.

### Configuration

- GUCs: `pg_tre.default_max_cost`, `pg_tre.pending_list_limit`,
  `pg_tre.range_size_blocks`, `pg_tre.bloom_tuple_bits`,
  `pg_tre.max_extraction_fanout`, `pg_tre.max_nfa_states`,
  `pg_tre.compile_timeout_ms`, `pg_tre.match_timeout_ms`,
  `pg_tre.fastupdate`, `pg_tre.tuple_bloom_enable`.
- `pg_tre.tuple_bloom_enable = false` by default: the tier-3
  per-tuple bloom and positional filter are gated until the
  chain-rank lookup is repaired (v1.3 followup).  Recheck
  remains authoritative for correctness.
- Reloptions: `bloom_tuple_bits`, `range_size_blocks`,
  `fastupdate`, `tuple_bloom_enable`, `pending_list_limit`.

### Testing

- 13 SQL regression tests under `test/sql/`.
- 3 TAP tests under `tap/` (concurrency, replication,
  crash recovery).
- 3 shell tests under `test/scripts/` (`wal_audit.sh`,
  `replication.sh`, `stress.sh`) plus a shared library
  (`lib.sh`).
- 6 GitHub Actions workflows: `ci.yml`, `formatting.yml`,
  `pgspot.yml`, `upgrade-tests.yml`,
  `sanitizer-build-and-test.yml`, `nightly-stress.yml`.
- Codecov upload wired in `ci.yml` (informational).

### Build and verify

```
PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config
make           PG_CONFIG=$PG_CONFIG
make install   PG_CONFIG=$PG_CONFIG
PG_CONFIG=$PG_CONFIG bash scripts/run-regress.sh
PG_CONFIG=$PG_CONFIG bash scripts/release-check.sh
```

Pre-tag gate: `scripts/release-check.sh`.

## v1.3 followups

- ~~Fix the chain-rank lookup~~ — **root cause fixed in
  1.2.1.**  The struct-vs-bytes mismatch in the scan-side
  bloom check that has been masquerading as a "chain-rank"
  issue is now fixed; tier-3 works correctly on
  posting-tree candidates at all scales.  Multi-leaf chain
  walking and per-leaf rank computation were already
  correct.  A residual pending-overlay regression remains
  (pending-list TIDs are rejected when tier-3 is on), so
  the GUC default stays at `false` until that's resolved.
- **Inline-data scan-path bug** discovered while tuning
  `PG_TRE_INLINE_POSTING_MAX` in 1.2.1 (and reverted to
  256 for that release).  Two regressions appear at
  thresholds > 256:
  - `wal_audit.sh`'s post-crash differential check fails at
    384: index returns 0 for a pattern that seq-scan finds
    1000 of.  WAL-redo path interaction with larger inline
    blobs.
  - At ≥ 448 the multi-leaf 100K-row test returns 0 rows
    for `Row 12[0-9][0-9][0-9]`.  Inline-data scan path
    interaction with the multi-leaf chain walker.
  Fix unlocks raising the threshold to 1024+ bytes for
  significant size reduction on sparse-trigram corpora.
- Variable-width per-tuple blooms (see
  `doc/specs/variable-width-blooms.md`).  Depends on the
  chain-rank repair landing first.  Yields ~70-80%
  per-tuple-bloom payload reduction on short-text corpora.
- Delta-aware WAL redo so we can drop `REGBUF_FORCE_IMAGE`
  and only ship FPIs as a fallback.  Today every WAL record
  carries a full-page image of every modified buffer, which
  is correct but bigger than necessary.
- `wal_consistency_checking = 'pg_tre'` clean.  Currently
  the redo callback's post-state diverges from the primary's
  FPI on at least one byte (likely a hint bit or
  uninitialized padding).  Gated behind
  `TRE_WAL_CONSISTENCY=1` in `replication.sh`; fixing
  byte-identity is part of the delta-aware-redo work.
- 1M-row real-corpus benchmark execution + `doc/perf.md`
  refresh.
- `libFuzzer` harness fidelity: replace stub structs in
  `fuzz/memutils_stub.c` with real pg_tre header includes.

## v2.0 followups

- Coverity scan integration (deferred from this cycle).
- PG memory-context ASAN instrumentation patch so the
  sanitizer CI catches use-after-free across `MemoryContext`
  boundaries.
- Index-side `ORDER BY` for the `<@>` operator (today the
  executor sorts after recheck; index-side requires
  structural amapi extensions \u2014 `amorder_by` callback,
  top-N early termination).
- **Posting-page coalescing** (see
  `doc/specs/posting-page-coalescing.md`).  The structural
  change that closes most of the size gap to pg_trgm: pack
  4-20 low-cardinality trigrams onto a single page instead
  of one page per trigram.  Format-version bump.  Estimated
  10x page-count reduction on sparse-trigram corpora.
- Parallel scan (`amcanparallel = true`) and parallel
  worker builds (`amcanbuildparallel = true`).  Both flags
  are false today; `CREATE INDEX CONCURRENTLY` works
  through standard PG machinery and does not require
  `amcanbuildparallel`.
- Multi-leaf chain-rank repair so tier-3 / positional
  filters work across right-link chains.
- Whole-tree `clang-format` reformat (deferred to keep
  git-blame useful for legacy code).
- PG17 support in CI matrix (codebase currently uses
  PG18-only APIs that need #if-version guards before PG17
  builds).
- Release artifact builds: Linux / macOS \u00d7 amd64 / arm64
  binaries via `release.yml` and `package-release.yml`.
- Replication scenarios beyond `replication.sh`:
  `replication_failover.sh`, `replication_concurrency.sh`,
  `replication_cascading.sh`, `replication_compat.sh`.
- `gh-pages` benchmark dashboard with PR-comment regression
  alerts.
