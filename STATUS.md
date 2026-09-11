# pg_tre status

Released: **3.2.5** (2026-09).  See `CHANGELOG.md` for full
release notes and `doc/design.md` for the architecture this
file tracks against.

3.2.5 fixes a regex-tokenizer bug: a literal `-` first or last in
a bracket expression (`[-_.]`, `[abc-]`, `[-]`, `[^-x]` -- all
valid POSIX) was rejected as an invalid pattern instead of
matching, so any query using such a class ERRORed on the index
path.  Found while reproducing a field report about casing; it is
a distinct bug and the one actually affecting that reporter.
Scan-path only: no on-disk change, no REINDEX.  The casing symptom
they reported does not reproduce on 3.2.4 -- the guard has been in
since 3.2.3, and their deployment is running a stale pin (3.0.1)
from a second container image.  See `CHANGELOG.md`.

3.2.4 is a packaging fix: v3.2.3 could not be built via the Nix
flake at all (flake.nix pinned a TRE rev older than the vendored
submodule, so the progress-hook patch failed to apply), and its
derivation carried a stale "3.0.2" version string.  A `make` build
from a git checkout was unaffected, which is why it got past
qualification -- `scripts/release-check.sh` now runs `nix build
.#pg18` and a flake/submodule rev comparison.  No C, SQL, WAL, or
on-disk change from 3.2.3; no REINDEX.  **Take 3.2.4 rather than
3.2.3.**

3.2.3 is a correctness release.  The v10 SuRF prefilter could
reject a case-insensitive anchored query (`~*` / `ILIKE` on a
`^prefix`) and return **zero rows** for a pattern with real
matches -- silently, with no error.  The prefix key is derived
from the pattern's literal codepoints with no case folding, while
the index stores trigrams case-sensitively, so `~* '^GIT'` looked
up a key an index holding `"git"` legitimately lacks.  Reported by
the solnix.io infra team against 3.2.2; the defect dates to the
v10 tier in 3.2.0.  Read-path only: no on-disk change, no
REINDEX.  Vendored TRE also moves to upstream master (18 commits
past v0.9.0) for integer-overflow, regex-length-limit and backref
hardening, with pg_tre's timeout-hook patch rebased and its DoS
guards re-verified.  Qualified on an i4i.8xlarge: the reported
failure was reproduced against the actual released 3.2.2 and
confirmed fixed by an in-place upgrade with no REINDEX (0 -> 3 rows,
matching seq scan), 41/41 regression tests, zero accuracy-oracle
mismatches across all stress scenarios, and the SuRF fast path
intact at 0.060 ms.  Results in
`bench/stress/RESULTS-stress-3.2.3.md`.  See `CHANGELOG.md`.

3.2.2 refreshes the vendored sparsemap from v5.1.1 to v5.5.0, an
upstream correctness release (seven bugs; three data-loss or
corruption on ordinary inputs).  Its release note claimed
"existing indexes remain readable" -- accurate about the sparsemap
wire format, but it did not cover case-insensitive queries; see
the correction in `CHANGELOG.md`.

3.2.1 is a qualification + documentation release: no C, SQL, WAL,
or on-disk-format change (metadata-only `UPDATE`, no REINDEX).
The 3.2 line was put through a full at-scale adverse-conditions
stress suite (`bench/stress/`, results in
`bench/stress/RESULTS-stress-3.2.0.md`) on an AWS i4i.8xlarge with
NVMe at 250k-10M rows: the accuracy oracle passed with zero
mismatches everywhere, crash-recovery-mid-build was clean, the
DoS guards held, and parallel builds had no deadlocks.  Two
non-blocking findings (low super-linear build throughput at scale;
posting-leaf bloat under sustained churn that REINDEX reclaims)
are documented in `LIMITATIONS.md`.

3.2.0 is a feature release on the 3.x lineage: on-disk format
bumps v9 -> v10 but stays backward-readable (min v6), so no
re-index is required (REINDEX populates the new filter on an
existing index).  Headline changes: (1) a **SuRF (Succinct
Range Filter)** tier -- a from-scratch, dependency-free C
reimplementation of SuRF-Base as a LOUDS-Sparse trie over an
order-preserving trigram key -- rejects `^`-anchored / prefix /
`LIKE 'foo%'` scans whose leading trigram is absent from the
whole index, without descending the posting tier or touching
the heap (~570x faster on an absent anchored prefix over a
500k-row index; one-sided error, never drops a true match);
(2) the vestigial BRIN-style range-bloom tier -- built on every
index but never read at scan time -- was **removed** (its GUC
and reloption are retained but ignored).  `tre_surf_stats()`
exposes the filter's size.

3.1.0 was a correctness + robustness release on the 3.0 lineage:
on-disk format unchanged (v9, min readable v6), no re-index
required.  Headline changes: (1) the `%~~` and `<@>` recheck now
honors a pattern's per-edit cost weights (`cost_ins`/`cost_del`/
`cost_subst`), matching the seq-scan UDF form; (2) approximate
(k>0) matching is UTF-8 codepoint-correct over multibyte text
(the k>0 tiling spine previously slid over raw bytes and dropped
codepoints > 0xFF, silently missing CJK/accented matches); (3)
real opclass validation (`amvalidate`) instead of the previous
always-true stub; (4) the KNN `ORDER BY <@>` path gains
`ammarkpos`/`amrestrpos`; (5) consistent WAL critical sections
across all page writers; (6) a PG18/PG19 build-compat include fix.
Parallel `CREATE INDEX` (`amcanbuildparallel`,
`pg_tre.enable_parallel_build` on by default) is now shipped and
enabled -- a leader plus background workers feed one coordinated
`tuplesort`, working for plain builds and `CREATE INDEX
CONCURRENTLY`.  `pg_tre_upgrade_index()` now covers the full
v6–v9 format range for in-place, no-REINDEX upgrades.

1.5.6 is a robustness + DoS-hardening release on the 1.5.0
lineage: same on-disk format (v5), no re-index required.
Headline changes: (1) the pending-list merge now descends a
MULTI-LEVEL upper tree, so once an index has grown an upper
internal page a subsequent insert+`VACUUM` merges cleanly
instead of erroring with a REINDEX demand; (2)
`materialize_merged_postings` iterates posting members in rank
order (`sm_next_member`) instead of probing every integer in
the TID range, eliminating a multi-minute ~100%-CPU spin on
wide-spanning trigrams; and (3) `pg_tre.compile_timeout_ms` is
now actually enforced via a real wall-clock deadline armed
around regex compilation.

1.5.5 is a storage-reclamation release on the 1.5.0 lineage:
same on-disk format (v5), no re-index required.  Headline
change: `VACUUM` now physically frees emptied out-of-line
posting leaves back to the index free-space map (the residual
left open in 1.5.4).  An emptied non-head leaf is spliced out
of its right-link chain and marked deleted with a deletion
XID; a later `VACUUM` cleanup pass recycles it into the FSM
once that XID has aged past the global visibility horizon
(nbtree-style deferred page deletion + recycle), so the blocks
are reused by future allocations instead of the index growing
until `REINDEX`.

1.5.4 is a correctness/hardening release on the 1.5.0
lineage: same on-disk format (v5), no re-index required.
Headline fixes: `ambulkdelete` now also cleans INLINE
postings stored directly in upper-tree leaf entries (it
rewrites the leaf's inline region in place), so `num_index_tuples`
is reported exactly; the compiled-NFA state count is guarded
against a corrupt/negative value that could otherwise bypass
the `pg_tre.max_nfa_states` DoS cap.

1.5.2 is a production-readiness audit patch on the 1.5.0
lineage: same on-disk format (v5), no re-index required.
Headline fixes: `pg_tre.match_timeout_ms` is now enforced
(progress hook into the vendored TRE matcher); `ambulkdelete`
reclaims dead TIDs on `VACUUM` instead of growing until
`REINDEX`; pending-list merge is atomic under one WAL record;
WAL `MarkBufferDirty`/`XLogRegisterBuffer` ordering and
scan-path memory safety hardened.

1.5.0 is a minor release on the 1.0.0 lineage: same on-disk
format, no re-index required.  Headline additions:

- **Similarity ranking** — `tre_distance(text, tre_pattern)`,
  `tre_similarity(text, tre_pattern)`, and the `<@>`
  ("eyeball") operator.  Inspired by `pg_textsearch`'s `<@>`
  for BM25 ranking.  Use
  `WHERE body %~~ pattern ORDER BY body <@> pattern ASC LIMIT N`
  to return the N closest matches.
- **Tier-3 per-tuple bloom — fully restored to default ON
  in 1.2.3.**  The struct-vs-bytes mismatch (1.2.2) plus the
  pending-overlay positional-filter fix (1.2.3) close out the
  long-running "chain-rank lookup repair" followup.  Tier-3
  works correctly across single-leaf, multi-leaf, and
  pending-overlay code paths at all tested scales (50 / 5K /
  100K rows).
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

## What ships in 1.5.2

### Storage and recovery

- Custom IndexAmRoutine registered as `USING tre`.
- On-disk format v5 (min readable v3): meta page, upper tree,
  multi-leaf posting trees with Lehman-Yao right-links,
  pending list, multi-leaf right-link-chained range tier,
  payload region.  v3/v4 indexes remain readable; use
  `pg_tre_upgrade_index()` to lazy-rewrite range pages to v5.
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
  `pg_tre.range_size_blocks`,
  `pg_tre.max_extraction_fanout`, `pg_tre.max_nfa_states`,
  `pg_tre.compile_timeout_ms`, `pg_tre.match_timeout_ms`,
  `pg_tre.min_trigram_freq`,
  `pg_tre.fastupdate`.
- The per-tuple positional bloom/payload path (the `tuple_bloom_enable`
  / `bloom_tuple_bits` GUCs + reloption, the `tier3_max_candidates`
  GUC, and the tier-3.1 positional pre-filter) was removed in 3.0.0.
  Builds emit payload-free posting leaves; the range bloom (tier-1
  skip) is retained.  Recheck remains authoritative for correctness.
- Reloptions: `q`, `range_size_blocks`, `fastupdate`,
  `pending_list_limit`.

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

- ~~Fix the chain-rank lookup~~ — **fully resolved in 1.2.3.**
  Two-part fix: 1.2.2 corrected a struct-vs-bytes mismatch in
  the scan-side bloom check; 1.2.3 corrected a positional-
  filter bug that dropped pending-list-only TIDs.  Tier-3 is
  back on by default and tested across single-leaf,
  multi-leaf, and pending-overlay code paths.  Multi-leaf
  chain walking and per-leaf rank computation were already
  correct.
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
  `doc/specs/variable-width-blooms.md`).  Tier-3 chain-rank
  prerequisite is now resolved (1.2.3); variable-width
  becomes an incremental size optimization on top of
  working tier-3.  Yields ~70-80% per-tuple-bloom payload
  reduction on short-text corpora.
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
- ~~Multi-leaf chain-rank repair~~ — **resolved in 1.2.3**
  (was a struct-vs-bytes bloom-header bug + a positional-
  filter bug, not a chain-rank issue).
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
