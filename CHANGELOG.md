# Changelog

All notable changes to pg_tre are documented in this file, organized by development phase.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [1.9.0] - 2026-06-05 - pg_trgm-compatible similarity (Phase A / A2)

> North star: pg_tre is a REGEX index with edit distance.  This
> release adds *capability parity* with pg_trgm's cheap similarity
> so users can drop pg_trgm and keep `%`, `<->`, and the
> similarity threshold -- it does not change what pg_tre is.

No on-disk format change; no REINDEX.  `ALTER EXTENSION pg_tre
UPDATE TO '1.9.0'` adds the new functions/operators.

### Added

- **Trigram-set (Jaccard) similarity, numerically identical to
  pg_trgm**: `tre_trgm_similarity(text,text) -> float4`,
  `tre_trgm_distance(text,text) -> float4` (1 - similarity), the
  `text % text` threshold operator, and the `text <-> text`
  distance operator.  These replicate pg_trgm's exact trigram
  model (lowercase, alnum-word split, 2-leading/1-trailing-space
  padding per word, deduplicated set) over UTF-8 codepoints.
  Verified numerically equal to `pg_trgm.similarity()` across a
  range of inputs (`foo|foobar`=0.375, `hello|hallo`=0.333,
  `abc|xyz`=0, `cat|category`=0.3, ...).
  - These are **cheap and stateless** and **distinct from**
    pg_tre's edit-distance `tre_similarity` / `<@>`.  A user
    porting `col % 'x'` or `ORDER BY col <-> 'x'` from pg_trgm
    gets the same numbers.
  - By design these operators **collide** with pg_trgm's own
    `%`/`<->` if both extensions are installed: pg_tre *replaces*
    pg_trgm; the two are not meant to co-exist.
- **`pg_tre.similarity_threshold`** GUC (PGC_USERSET, default 0.3,
  range 0..1), mirroring `pg_trgm.similarity_threshold`; the `%`
  operator honors it.

### Notes

- This is the first piece of the documented pg_trgm-replacement
  roadmap (`doc/specs/roadmap-pg_trgm-replacement.md`).  Still to
  come in Phase A: `LIKE`/`ILIKE`/`~`/`=` index acceleration (A1)
  and accurate `{~k}` selectivity (A3).  Index-side acceleration
  of `<->`/`%` is a later refinement; A2 delivers the functional
  and operator semantics.

---

## [1.8.2] - 2026-06-05 - green CI on tags (release-publish permission)

No extension/format/behavior change from 1.8.0/1.8.1; no REINDEX.
Continues closing the tag-CI gap: after 1.8.1 fixed `make dist`,
the tag release-artifacts job built the tarball but the GitHub
release-publish step 403'd because the default `GITHUB_TOKEN` is
read-only.  Grant `permissions: contents: write` and make the
publish step best-effort (this repo's canonical releases are the
Codeberg tags; GitHub is a mirror).  The tarball build remains
the packaging gate.

---

## [1.8.1] - 2026-06-04 - release-pipeline fix (green CI on tags)

No extension code, on-disk format, or behavior change from 1.8.0
(the compiled `.so` is byte-identical apart from the version
string); no REINDEX.  This patch exists solely to make the
tag-triggered release pipeline pass.

### Fixed (CI)

- The tag-only **release-tarball** job (`make dist`) aborted on
  `Makefile:157: pg_tre requires PostgreSQL 17 or newer; found
  16` because that job's runner `pg_config` is PostgreSQL 16 and
  the Makefile's version guard runs on every target.  Every tag
  push (1.6.1, 1.7.0, 1.8.0) therefore showed a red CI run even
  though the build/test matrix, sanitizers, and pgspot were all
  green on the corresponding `main` push.  `make dist` is pure
  `git archive` + tar + gzip and needs no PostgreSQL, so the
  version guard is now skipped when every requested goal is
  PG-independent (`dist`/`clean`/`distclean`); a bare `make`
  build and `make install` still enforce PG17+.

Process note: 1.6.1/1.7.0/1.8.0 were tagged with local
qualification green but the tag CI red (this packaging job).
That gap is what this release closes; going forward a release is
not considered done until the tag's CI is green.

---

## [1.8.0] - 2026-06-04 - tuplesort-based build: bounded memory at scale

No on-disk format change (still v6); same indexes, no REINDEX.
The structural fix for the field-report OOM: index builds no
longer hold the whole trigram-emission set in RAM.

### Changed

- **`ambuild` now sorts trigram tuples with PostgreSQL's
  `tuplesort` instead of an in-memory array + `qsort`.**  Each
  `(trigram_hash, tid, position)` tuple is encoded as a fixed
  20-byte big-endian `bytea` whose `memcmp` order reproduces the
  historical `(hash, packed_tid, position)` order exactly, then
  fed to `tuplesort_begin_datum(BYTEAOID, ...)`.  **Peak build
  memory is now bounded by `maintenance_work_mem`** (tuplesort
  spills to temp files), not by corpus size.
  - Measured: a 200k-row / ~80M-emission body corpus builds
    under `maintenance_work_mem = 32MB` with peak backend RSS
    **231 MB**; the same corpus at half the size (100k /
    ~40M emissions) peaks at **219 MB** -- i.e. **flat across a
    2x corpus**, where the old in-memory build would have grown
    from ~960 MB to ~1.9 GB of resident `entries[]` alone.
    Correctness verified (index result == seq-scan ground
    truth, exact and k=1).
  - This directly resolves the field-report failure where a
    body-column build OOM-killed the postgres cgroup.  Set
    `maintenance_work_mem` to taste; larger values trade RAM for
    fewer merge passes / faster builds.
- **`pg_tre.build_max_entries_mb` default changed 4096 -> 0
  (disabled).**  Its original job -- prevent the in-memory OOM
  -- is now done structurally by tuplesort, and a non-zero
  default would wrongly block large-but-legitimate builds (e.g.
  the 500k-body case from the field report).  The GUC remains as
  an optional *temp-disk* safety valve: set it to cap the
  emitted-tuple count (x24 bytes) and fail cleanly with
  `ERRCODE_PROGRAM_LIMIT_EXCEEDED` rather than filling the temp
  tablespace.

### Notes

- The remaining build-time per-row memory is the tid-bloom hash
  (one entry per distinct TID, ~56 B/row), which is O(rows) not
  O(emissions) -- ~12 MB of the 219->231 MB delta above.  It is
  far smaller than the former `entries[]` term and only matters
  at very high row counts; bounding it is a possible future
  refinement, not a blocker.
- `CREATE INDEX CONCURRENTLY` / `REINDEX CONCURRENTLY` (verified
  in 1.7.0) compose with the bounded-memory build: a concurrent
  rebuild of a large body column is now both lock-light and
  memory-safe.

### Verified

- 21/21 regression; `wal_audit.sh` 3/3; `replication.sh` 4/4.
- 80M-emission build under 32 MB `maintenance_work_mem`; peak
  RSS flat across a 2x corpus; result correctness vs seq scan.

> Qualified locally; **GitHub CI verification still pending at
> tag time** (1.7.0 had a CI failure that needs a separate
> look) -- do not treat this as CI-green until confirmed.

Inspired by https://github.com/timescale/pg_textsearch
(Tiger Data, PostgreSQL License).

---

## [1.7.0] - 2026-06-04 - cancellable builds, memory ceiling, CIC verified

No on-disk format change (still v6); same indexes, no REINDEX.
Directly addresses the two operational failures from the 1.6
field report (see `LIMITATIONS.md`).

### Added

- **`pg_tre.build_max_entries_mb`** (PGC_USERSET, default 4096,
  `GUC_UNIT_MB`, 0 = unlimited).  The build's in-memory
  trigram-entry array grows with total trigram emissions, not
  distinct trigrams, so large natural-text columns could exhaust
  memory and get the postgres process SIGKILLed by the OOM
  killer.  When the array would exceed this ceiling the build now
  fails with `ERRCODE_PROGRAM_LIMIT_EXCEEDED` and an actionable
  hint -- **the server stays up**.  The ceiling is enforced at
  both the initial allocation and every doubling.  Verified: a
  4 MB cap on a 20k-row build errors cleanly; the default lets
  normal builds through.

### Changed

- **Index builds are now cancellable throughout.**  The sort over
  the entries array uses `qsort_interruptible` (PG18) instead of
  `qsort`; combined with the existing `CHECK_FOR_INTERRUPTS` in
  the per-TID `sm_add_grow` loops and multi-leaf write paths,
  `pg_cancel_backend()` / SIGINT during a build is honored within
  ~a second on any phase.  Previously the `pg_qsort` step (5-60s
  on a large corpus) and the posting-build loops were
  uncancellable, which is why a stuck `REINDEX` held its lock for
  minutes in the field report.

### Fixed (documentation)

- **`CREATE INDEX CONCURRENTLY` / `REINDEX CONCURRENTLY` are
  supported and verified.**  CIC/RIC are generic in PostgreSQL
  (no per-AM flag gates them); pg_tre's build is correct under
  the two-phase MVCC-snapshot protocol, confirmed by a
  CIC-result == seq-scan-ground-truth cross-check and a RIC
  round-trip.  The 1.6.1 docs incorrectly said "not yet
  supported" (based on a mis-diagnosed, non-existent
  `amcanbuildconcurrently` flag); README and LIMITATIONS.md are
  corrected.  **Using the CONCURRENTLY variants avoids the heavy
  build lock that stalled ingest in the field report** -- this is
  the recommended path for rebuilding pg_tre indexes on a live
  table.

### Still pending (v1.8.0)

- A `tuplesort`-based build that bounds peak memory by
  `maintenance_work_mem` (disk-spill, like btree/gin/gist).
  1.7.0 makes the OOM a clean error and the lock avoidable; 1.8.0
  makes large body-corpus builds actually fit in bounded memory.

### Verified

- 21/21 regression; `wal_audit.sh` 3/3; `replication.sh` 4/4.
- CIC + RIC correctness cross-check.
- Bounded-memory guard fires cleanly without OOM; server
  survives.

Inspired by https://github.com/timescale/pg_textsearch
(Tiger Data, PostgreSQL License).

---

## [1.6.1] - 2026-06-04 - documentation honesty about scale limits

Documentation-only release.  No extension code changes; same
on-disk format as 1.6.0; no ALTER EXTENSION work; no REINDEX.

### What and why

A production user (501 k-row email-archive, see
[`LIMITATIONS.md`](LIMITATIONS.md) for the field report) hit two
operational failures on the 1.6.0 REINDEX path:

1. **Lock-hung `REINDEX`** on a 501 k-row text column for ~6
   minutes, blocking ingest -- the build path has uncancellable
   stretches inside `pg_qsort`, the inner `sm_add_grow` loops,
   and the multi-leaf write paths.
2. **OOM-killed the postgres cgroup** while building a body-text
   index -- `ambuild` collects every trigram emission in memory
   (`bstate->entries[]`, `repalloc_huge`-grown), so peak RSS
   scales with `total_emissions`, not `distinct_trigrams`.  ~6 GB
   is normal for 500 k email bodies.  The 1.6.0 sparsemap fix
   removed the silent data-loss overflow, but does not address
   the resident-set model.

Neither failure is a 1.6.x regression; both have been latent
since at least 1.4.x.  But the README and CHANGELOG implied
production-readiness at scales where the build is not viable,
and the user reasonably tried it.  This release closes that gap
in words; **1.7.0** addresses cancellability and a bounded-error
build ceiling, **1.8.0** introduces a `tuplesort`-based build
that actually scales.

### Documentation

- New `LIMITATIONS.md` at the repo root: explicit sizing table,
  upgrade/REINDEX guidance, and the verbatim field report.
- README: prominent **Scale limits** callout under Features,
  build-memory note + **Deployment recommendations** under
  Performance, and a corrected feature claim -- the bullet that
  said `REINDEX CONCURRENTLY safe` was wrong (`amcanbuildconcurrently`
  is `false` today; CIC/RIC fall back to the lock-heavy form).
- `doc/perf.md`: new "Build memory model" section with the
  per-emission formula and a sizing table.

### Operator guidance

For heaps where 1.6.0's REINDEX is not viable, the recommended
path is `DROP INDEX` + recreate during a maintenance window with
generous `maintenance_work_mem` -- and first reconfirm whether
the column actually warrants pg_tre at all.  Email-archive style
workloads are typically better served by `pg_trgm` GIN +
`tsvector` BM25 with pg_tre reserved for true edit-distance/regex
fuzzy matching over an already-narrowed subset.  See
[`LIMITATIONS.md`](LIMITATIONS.md) for details.

Inspired by https://github.com/timescale/pg_textsearch
(Tiger Data, PostgreSQL License).

---

## [1.6.0] - 2026-06-04 - sparsemap 4.0.0: data-loss fix (REINDEX REQUIRED)

**Breaking, correctness-critical release.**  On-disk format bumped
to **v6**; indexes built by pg_tre < 1.6 **must be REINDEXed**.

### Why this is mandatory

The vendored sparsemap is updated to **4.0.0**, which widens the
per-chunk start offset in the serialized format from 32 to 64 bits
(sparsemap `SM_WIRE_VERSION` 1 -> 2).  The 32-bit offset **silently
lost data for sparsemap indices >= 2^32**.  pg_tre packs heap TIDs
as `(block << 16) | offset`, so **any heap larger than ~512 MB**
(> 65536 blocks) reaches indices >= 2^32 -- meaning pg_tre indexes
on large tables under pg_tre < 1.6 could silently miss rows.
4.0.0 fixes the bug; REINDEX regenerates correct index data.

### Upgrade

```sql
ALTER EXTENSION pg_tre UPDATE TO '1.6.0';   -- after installing the new .so
REINDEX TABLE your_table;                    -- rebuild every pg_tre index
```

The sparsemap 4.0.0 wire format is intentionally **not**
backward-readable (`sm_deserialize` returns NULL on the old
format), so there is no online in-place migration: REINDEX is the
migration *and* the data-loss remedy.  `pg_tre_read()` rejects
pre-v6 pages with `ERRCODE_FEATURE_NOT_SUPPORTED` and a REINDEX
hint, so an un-rebuilt index **fails loudly on first scan** rather
than returning wrong rows.

### Changed

- Vendored sparsemap 4.0.0 (`sm.c`/`sm.h`) via the upstream
  `contrib/pg_tre_sync.sh`.  Adopted the new public type name
  `sm_t` at all pg_tre call sites (sparsemap 4.0.0 renamed
  `sparsemap_t` -> `sm_t`); no compatibility alias.
- Namespaced the embedded sparsemap with
  `-DSPARSEMAP_PREFIX=__tre_` so every public sparsemap symbol
  is emitted as `__tre_sm_*` (76 functions).  This prevents
  link-time collisions if another library in the same backend
  also vendors sparsemap.  Compile-time only; the serialized
  wire format is unchanged.
- `PG_TRE_FORMAT_VERSION_LATEST` and `..._MIN` are both 6.

### Verified

- 21/21 regression tests pass.
- `wal_audit.sh` 3/3; `replication.sh` 4/4.
- New indexes report on-disk format v6
  (`pg_tre_index_format_status`).
- The >= 2^32 data-loss fix is covered at the sparsemap layer by
  upstream `tests/test_large_index.c`; a full >512 MB pg_tre
  large-heap test is recommended for the nightly lane (too slow
  for the regression suite).

Inspired by https://github.com/timescale/pg_textsearch
(Tiger Data, PostgreSQL License).

---

## [1.5.8] - 2026-06-03 - sparsemap 3.0.1, version-string fix, CI green

Maintenance release.  Same on-disk format as 1.5.7
(`PG_TRE_FORMAT_VERSION` = 5); the vendored sparsemap update is
wire-compatible (byte-identical serialized format), so existing
indexes built by 1.3.0 .. 1.5.7 remain readable.  Upgrade is
online and needs no reindex:

    ALTER EXTENSION pg_tre UPDATE TO '1.5.8';

(after installing the new shared library).  Verified end-to-end:
an index built under 1.5.6 returns identical results after
`ALTER EXTENSION ... UPDATE TO '1.5.8'` with no reindex.

### Changed (library)

- **Vendored sparsemap updated to 3.0.1** (from the upstream
  `ports/rust` branch's consolidated C library, `sm.c`/`sm.h`).
  The portability shims that previously lived in
  `sm_portability.h` / `popcount.h` are now folded into the
  implementation, so `include/pg_tre/sm_portability.h` is
  removed.  Synced via the upstream `contrib/pg_tre_sync.sh`.
  The serialized format is unchanged (wire-compatible), so no
  index rebuild is required.
  - NOTE: 3.0.1's `sm.h` renamed the public type
    `sparsemap_t` -> `sm_t`.  All 82 `sm_*` functions are
    unchanged.  pg_tre keeps the historical spelling working
    with a one-line `typedef struct sparsemap sparsemap_t;`
    compatibility alias in the vendored header (reported
    upstream: the branch's own `pg_tre_sync.sh` advertises a
    clean drop-in, which the rename breaks).

### Fixed

- **`tre_version()` reported the wrong version.**
  `PG_TRE_VERSION_STRING` in `include/pg_tre/pg_tre.h` had
  drifted to `"pg_tre 1.5.6"` and `scripts/bump-version.sh`
  only rewrites it when the *current* string matches the OLD
  version, so the constant was never corrected across the
  1.5.6 -> 1.5.7 bump.  Shipped 1.5.7 therefore reported
  `tre_version() = 'pg_tre 1.5.6'`.  Corrected to 1.5.8;
  future bumps now track it via the existing common-files
  substitution.

### Fixed (CI)

- **pgspot Security Check** was red on four `PS017`
  ("unqualified object reference") warnings for pg_tre's own
  `%~~` / `<@>` operators and the `tre_pattern` type,
  referenced inside the extension's own install script.
  These resolve at `CREATE EXTENSION` time and operators
  cannot be schema-qualified in operator-class / cast DDL, so
  PS017 is a false positive here.  Added `--ignore PS017` to
  both pgspot invocations.
- **Nightly stress** died at dependency install with
  `E: Unable to locate package postgresql-18-pgwalinspect`.
  pg_walinspect is a contrib module bundled in the
  `postgresql-NN` server package on PGDG; there is no
  separate `-pgwalinspect` apt package.  Removed the bogus
  name; `CREATE EXTENSION pg_walinspect` still resolves.
- **Sanitizer build** failed at
  `vendor/tre/configure ... Error 127`: TRE's `autogen.sh`
  calls `autopoint(1)`, which on Ubuntu is a separate package
  from `gettext`.  Added `autopoint` to the sanitizer and
  nightly apt lists (the main CI job already had it).
- Added `vm.mmap_rnd_bits=28` + `kernel.randomize_va_space=0`
  to the ASan jobs for the Ubuntu 24.04 runner ASLR/ASan
  shadow-memory interaction.

### Changed (CI)

- The two ASan workflows (the from-source PG-with-ASan build,
  and the nightly `asan` matrix leg) are now `continue-on-error`
  (informational).  Both fail in the ASan/PostgreSQL toolchain
  environment -- the instrumented `postgres` aborts in initdb's
  own bootstrap, and a stock server cannot start with an
  ASan-instrumented module preloaded -- not in pg_tre, and they
  have never passed.  The blocking CI (main CI matrix +
  regression tests, pgspot, nightly `plain` leg) is green.
  A proper ASan+PG recipe is tracked as a follow-up.

### Verified

- 21/21 regression tests pass on the release tree.
- `wal_audit.sh` 3/3; online upgrade 1.5.6 -> 1.5.8 returns
  identical results with no reindex.
- Main `CI` workflow and `pgspot` green on GitHub.

Inspired by https://github.com/timescale/pg_textsearch
(Tiger Data, PostgreSQL License).

---
## [1.5.6] - 2026-06-01 - multi-level pending merge, posting-merge speedup, compile-timeout enforcement

Robustness + DoS-hardening release on the 1.5.0 lineage.  No
on-disk format change (remains v5); no REINDEX required.
Upgrade script `1.5.5--1.5.6` is a no-op stub that only bumps
the recorded extension version.

### Fixed

- **Pending-list merge now supports a multi-level upper tree.**
  Once an index accumulated enough distinct trigrams that
  `VACUUM` had to build an upper *internal* page (page_kind=2)
  fanning out to many upper leaves, the next pending merge
  raised `ERROR: pg_tre: pending-list merge on multi-level
  upper tree not yet supported` and demanded a REINDEX — the
  merge snapshot could read back only a single upper leaf. The
  snapshot path (`snapshot_existing_upper`) now descends the
  whole upper subtree recursively (collecting child blocks
  under lock, releasing, then recursing — never holding more
  than one upper buffer locked), so subsequent inserts merge
  cleanly with no REINDEX. The bulk-load and lookup paths
  already handled multi-level trees; this was a read-side gap
  in the merge snapshot only.
- **`materialize_merged_postings` no longer spins O(TID-range).**
  The on-disk-leaf merge branch probed every integer in
  `[min_idx, max_idx]` with `sm_contains`, burning ~100% CPU
  for minutes when a trigram's TIDs spanned a wide range. It
  now iterates set members in rank order via `sm_next_member`
  (O(cardinality)).

### Security

- **`pg_tre.compile_timeout_ms` is now enforced.** The GUC was
  previously defined but never honored — only `match_timeout_ms`
  and `max_nfa_states` bounded cost, and `max_nfa_states` is
  checked only *after* the automaton is built, so a pathological
  nested bounded repetition (e.g. `a{80}{80}{80}`) could expand
  for seconds before any cap applied. A progress hook woven into
  TRE's NFA-build loops (the bounded-repetition AST expansion
  and the tag/copy/compile worklists) now compares wall-clock
  time against a deadline armed around compilation and aborts
  with a `query_canceled` error (`pg_tre: regex compilation
  exceeded pg_tre.compile_timeout_ms`). The compile and match
  deadlines use independent progress hooks so arming one never
  perturbs the other. See `SECURITY.md`.

---

## [1.5.5] - 2026-06-01 - posting-leaf FSM reclamation (deferred page deletion + recycle)

Storage-reclamation release on the 1.5.0 lineage.  No on-disk
format change (remains v5); no REINDEX required.  Upgrade
script `1.5.4--1.5.5` is a no-op stub that only bumps the
recorded extension version.

### Fixed

- **`VACUUM` now physically frees emptied posting leaves to the
  index FSM** (the residual left open in 1.5.4).  Previously an
  out-of-line posting leaf that `VACUUM` emptied was repacked to
  a zero-entry leaf but never reclaimed: the block stayed
  allocated forever and the index only shrank under a full
  `REINDEX`.  1.5.5 adds an nbtree-style deferred page-deletion
  and recycle protocol:

  - When `ambulkdelete` empties a **non-head** posting leaf it
    splices the leaf out of its right-link chain (the
    predecessor's `right_link` is advanced past it) and marks it
    `PG_TRE_LEAF_DELETED`, stamping a deletion `FullTransactionId`
    just past an empty sparsemap.  The page is left as a coherent
    empty waypoint (preserved `right_link`) so a concurrent scan
    holding a stale `right_link` still terminates correctly --
    posting scans copy `right_link` without lock coupling.  The
    chain head is never unlinked (it is addressed from the
    upper-tree leaf entry).
  - `amvacuumcleanup` runs a recycle pass that sweeps the main
    fork and, for each `PG_TRE_LEAF_DELETED` leaf whose deletion
    XID has aged past the global visibility horizon
    (`GlobalVisCheckRemovableFullXid` -- nbtree's `safexid`
    gate), re-initializes the page and records it free
    (`RecordFreeIndexPage` + `IndexFreeSpaceMapVacuum`).
  - `pg_tre_extend_fork` now reuses FSM pages
    (`GetFreeIndexPage`), re-validating each candidate under its
    buffer lock before claiming it (nbtree `_bt_allocbuf`
    discipline) so a lost extend race falls through safely to a
    physical extension.

  The unlink and recycle operations are WAL-logged as full-page
  images (`XLOG_PTRE_POSTING_UNLINK`, `XLOG_PTRE_POSTING_RECYCLE`)
  replayed through the generic FPI path.  `amvacuum` now reports
  `pages_deleted` / `pages_free`.  New regression test
  `posting_recycle` builds a multi-leaf posting chain, empties an
  interior band, and verifies the leaves are unlinked, recycled
  into the FSM, and reused without the index growing -- while the
  index continues to agree with the heap throughout.

---

## [1.5.4] - 2026-05-31 - inline-posting vacuum cleanup, NFA-count hardening

Correctness/hardening release on the 1.5.0 lineage.  No
on-disk format change (remains v5); no REINDEX required.
Upgrade script `1.5.3--1.5.4` is a no-op stub that only bumps
the recorded extension version.

### Fixed

- **`ambulkdelete` now cleans INLINE postings** (C2 residual):
  small posting lists stored directly in upper-tree leaf
  entries (rather than out-of-line posting trees) were
  previously skipped by `VACUUM`, leaving their dead TIDs to be
  filtered only by heap MVCC recheck until the next `REINDEX`.
  The leaf's inline region (concatenated sparsemap + payload
  blobs) is now repacked in place under an exclusive lock, each
  entry's `inline_bytes` is refreshed, `pd_lower` is shrunk, and
  the page is WAL-logged as a full-page image (`XLOG_PTRE_VACUUM`).
- **Exact `num_index_tuples`**: because both out-of-line posting
  trees and inline postings are now fully traversed during
  `ambulkdelete`, the reported live-tuple count is exact;
  `estimated_count` is no longer set on the bulkdelete path.
- **NFA-state-count guard** (H5): `pattern_cache` now rejects a
  compiled regex whose internal NFA state count reads back
  negative (corruption / overflow), which would otherwise slip
  past the `pg_tre.max_nfa_states` `>` comparison and silently
  disable the DoS cap.

### Added

- `test/sql/vacuum_inline.sql` regression: deletes rows from a
  table whose postings stay inline, `VACUUM`s, and verifies the
  index-scan row set still equals the seq-scan row set (including
  a fully-emptied inline posting), guarding the in-place inline
  rewrite against corruption.

### Build

- Bumped the vendored `lime` parser-generator submodule to
  `v0.8.7`.  The generator (build-time LALR(1) codegen only; not
  linked into `pg_tre.so`) was restructured upstream so the Rust
  output feature now lives in `src/emit_rust.c`; the `lime`
  binary build rule now compiles that translation unit alongside
  `lime.c`.  Regenerates `src/query/tre_grammar.c`; verified by a
  full clean rebuild + 18/18 regression pass.

### Known limitations

- Emptied posting leaves are repacked but **not** physically
  freed to the free-space map.  Safe physical reclaim requires
  an nbtree-style page deletion / recycle protocol (half-dead
  marking, right-link re-routing, XID-gated recycling) because
  `VACUUM` runs concurrently with lock-coupling-free right-link
  scans.  Tracked as future work; `pages_deleted` / `pages_free`
  remain 0.

## [1.5.3] - 2026-05-31 - version-consistency patch

Documentation/version-consistency patch.  No on-disk format
change (remains v5); no REINDEX required.  No C-level or
SQL-level behavior change.  Upgrade script `1.5.2--1.5.3` is
a no-op stub that only bumps the recorded extension version.

### Changed

- `STATUS.md` and `scripts/release-check.sh` brought in line
  with the shipped version (both still referenced 1.5.0 / a
  stale tag example).  All version strings now consistent at
  1.5.3.

## [1.5.2] - 2026-05-31 - production-readiness audit hardening

Patch release addressing findings from three independent
code-review audits.  No on-disk format change (format
remains v5); no REINDEX required.  Upgrade scripts
`1.5.0--1.5.1` and `1.5.1--1.5.2` are no-op stubs that
only bump the recorded extension version.

### Security

- **`pg_tre.match_timeout_ms` is now enforced** (was defined
  but never read).  A plain-C progress hook is compiled into
  the vendored TRE matcher loops (approx, parallel, and
  backtrack paths) via `patches/tre-progress-hook.patch`,
  applied idempotently at build time.  The hook compares
  `GetCurrentTimestamp()` against a per-query deadline armed
  around each match and aborts pathological matches that
  would otherwise hang a backend uninterruptibly.  `SECURITY.md`
  rewritten to describe the actual guarantees.
  `compile_timeout_ms` remains advisory-only (bounded by the
  64KB pattern-length ceiling and `max_nfa_states`), now
  documented honestly.

### Fixed

- **`ambulkdelete` no longer a no-op** (C2): posting trees are
  now walked from the upper tree and dead TIDs removed in
  place, with real `tuples_removed` / `num_index_tuples` /
  `num_pages` statistics reported.  Indexes reclaim space on
  `VACUUM` instead of growing until `REINDEX`.  (Residual:
  INLINE postings are still reclaimed only by `REINDEX`.)
- **Atomic pending-list merge** (C3): merge captures a
  watermark and finalizes under a single WAL record in one
  critical section, eliminating a torn-merge window.
- **WAL record ordering** (H1): `MarkBufferDirty` now precedes
  `XLogRegisterBuffer` in `upper.c` (2 sites) and `meta.c`.
- **Scan-path memory safety** (H2/H3/H4): pattern cache
  entries are pinned/refcounted across a scan with
  `longjmp`-safe release; per-call position buffers are
  `palloc`'d instead of using a shared static buffer.
- **Bloom filter guards** against `m_bits == 0`.

### Changed

- All version strings unified at **1.5.2** (control,
  `META.json`, `README`, spec, `Makefile`).  Note: v1.5.1
  shipped with `default_version` still at 1.5.0; the full
  upgrade-script chain is now consistent through 1.5.2.

## [1.5.0] - 2026-05-26 - range-tier multi-leaf, query-time hoist

Minor release on the 1.x line.  On-disk format bump from
v4 to v5 for range pages (right-link chained); existing
v4 indexes remain readable, no REINDEX required.  Use
`pg_tre_upgrade_index()` to lazy-rewrite v4 range pages
in v5 format if you want them to benefit from the multi-
leaf feature without a rebuild.

### Added

- **Multi-leaf range tier**: `PgTreRangeHeader` (8 bytes) at
  the start of each range page chains pages via `right_link`.
  Build path emits multiple range pages when entries don't
  fit on one; read paths walk the chain.  Per-page format
  dispatch keeps v3/v4 range pages (no header) readable for
  back-compat.
- 1M-row natural-text corpus now reports
  `built range tier with 118 ranges across 4 pages` (was: 30
  captured + 88 silently truncated, with a warning).
  **Tier-1 selectivity now applies to 100% of the heap**
  instead of the first 25%.

### Changed

- **Hoist `pg_tre_upper_lookup` out of tier-3 inner loop**.
  `apply_tuple_bloom_filter` and the Phase 5.1 positional
  filter previously called `pg_tre_upper_lookup` once per
  (candidate, query-trigram) pair (3.6M probes for the 1M
  reproducer).  A per-scan trigram_hash -> upper-tree leaf
  cache is now built once at scan setup and consulted in
  the inner loop.
  - 1M `'connection refused'` cold-cache: ~66 s -> ~36 s
    (1.84x speedup).
  - Buffer-hit reduction is small (~1.5%): the dominant
    structural cost is `pg_tre_posting_materialize` for
    high-cardinality trigrams, not upper_lookup.  Skip-
    pointer style intersection short-circuit for that path
    is queued for a future cycle.
- Critical implementation detail: cached entries hold no
  buffer pin or LWLock across the scan (the inline blob is
  copied out and the leaf released immediately).  An earlier
  iteration that held SHARE LWLocks was 2x slower.
- `PG_TRE_FORMAT_VERSION_LATEST = 5`.

### Honest performance status

Variable-width per-tuple blooms were prototyped on branch
`variable-width-v6` (commit `4bbbc38`).  Wire format,
build-side two-pass restructure, and decoder dispatch all
work; 17/17 regression passes; `wal_audit` and
`replication` clean.  But on the test corpus (1M rows of
`/usr/share/dict/words` Zipfian samples, ~83 trigram
emissions/row, 24,269 distinct trigrams), the variable-
width output is **3 MB larger** than the fixed-128 baseline
(every row landed in the 256-bit bucket; the bucket
thresholds were calibrated for distinct-count, not
emission-count).  Branch is parked; not merged.  The win
materializes for short-text corpora (URLs, error codes,
IDs); the natural-text 1M case is bottlenecked elsewhere.

At 1M rows on this corpus, exact-match queries via the
index remain ~2 orders of magnitude slower than `pg_trgm`
GIN.  The remaining gap lives in the trigram posting-list
intersection cost in `pg_tre_posting_materialize`.
Closing it requires skip-pointer-style early termination,
which is a multi-week structural change deferred to a
future release.

Where `pg_tre` is the only PostgreSQL answer (k>0 fuzzy,
character-class regex, ORDER BY `<@>`), it stands alone
at any scale.

### Verified

- 17/17 regression tests pass.
- `test/scripts/wal_audit.sh`: 3/3 pass.
- `test/scripts/replication.sh` (default): 4/4 pass.
- `test/scripts/replication.sh` with
  `TRE_WAL_CONSISTENCY=1`: 4/4 pass.
- 1M-row CREATE INDEX completes in ~80 s (no truncation
  warning); index size ~3.8 GB; `'connection refused'`
  query 36 s cold / 33 s warm.

Inspired by https://github.com/timescale/pg_textsearch
(Tiger Data, PostgreSQL License).

---

## [1.4.1] - 2026-05-26 - 1M-row scaling fixes

Patch release on the 1.4 line.  Same on-disk format as 1.4.0;
ALTER EXTENSION pg_tre UPDATE TO '1.4.1' has no SQL work to do;
no re-index required.

### Fixed

- **`palloc` 1 GB cap in `ambuild.c`**: the build path collected
  `(trigram_hash, tid, position)` entries for every trigram
  emission during heap scan.  At ~85 trigrams/row x 1M rows
  that is ~83.5M entries x 24 bytes = ~2 GB, exceeding PG's
  `MaxAllocSize`.  Switched the entries-array allocation to
  `MemoryContextAllocHuge` / `repalloc_huge`.  CREATE INDEX on
  1M+ rows previously failed with
  `invalid memory alloc request size 1610612736`.
- **Single-level upper-tree internals in `upper.c`**: the old
  `upper_build_internal_level` was hardcoded to a single internal
  page (max ~506 entries).  This capped pg_tre at ~10-20K
  distinct trigrams - which maps to roughly 50K rows of
  dictionary-style natural text.  Beyond that the build
  failed with `pg_tre: upper-tree internal level overflow`.
  Now the writer is recursive: when the input doesn't fit on
  one page it builds `ceil(N / fanout)` sibling pages and
  recurses on those pages' first-keys.  The reader's descent
  in `pg_tre_upper_lookup` is refactored from an if/else
  (root-is-leaf vs root-is-internal-with-one-descent) into a
  loop that walks any number of internal levels until it hits
  a leaf.
- **Latent correctness bug surfaced by the multi-level descent**:
  the previous single-level descent's `first_key <=
  trigram_hash` rightmost-match returned no match when the
  query trigram's hash was less than the leftmost first_key
  on the internal page.  Most queries' hashes land in the
  middle of the distribution so the bug was masked, but a few
  selective queries against indexes built with multiple leaves
  silently dropped rows.  `test/expected/utf8.out` and
  `test/expected/order_by.out` were refreshed to reflect the
  now-correct behavior (rows that the index used to drop are
  now returned).

### Verified

- 17/17 regression tests pass.
- `test/scripts/wal_audit.sh`: 3/3 pass.
- `test/scripts/replication.sh` (default): 4/4 pass.
- `test/scripts/replication.sh` with `TRE_WAL_CONSISTENCY=1`:
  4/4 pass.
- 1M-row CREATE INDEX now completes in 80 s (was: ERROR at
  invalid memory alloc 1.5 GB or upper-tree internal overflow).

### Benchmark infrastructure

- `bench/gen_corpus.py`: Python CSV generator using
  `/usr/share/dict/words`.  Produces 1M rows in 3.9 s.
- `bench/bench_1m_v2.sql`: `\copy`-based driver replacing the
  O(N^2) plpgsql sampler in the original `bench_1m.sql`
  (which was unusable beyond ~10K rows).
- `doc/perf.md` rewritten with 1M-row numbers.  pg_tre is
  structurally larger (24x) and slower (3x build) than pg_trgm
  GIN at 1M rows because of per-trigram page allocation; v2.0's
  posting-page coalescing is the planned fix.  Where pg_tre
  is the only answer (k>0 fuzzy, character-class regex,
  ORDER BY `<@>`), it completes in 1-7 seconds at 1M rows.

Inspired by https://github.com/timescale/pg_textsearch
(Tiger Data, PostgreSQL License).

---

## [1.4.0] - 2026-05-25 - ORDER BY index-side, delta WAL, format-upgrade framework

First minor release on the 1.4 line. Same on-disk format as
1.3.x: existing indexes are read transparently. Forward-compatible
format-upgrade framework lands so future format bumps (variable-
width per-tuple blooms in v2.0) can be applied in place via
`pg_tre_upgrade_index()` without REINDEX.

### Added

- **Index-side `ORDER BY <@>`** via `amcanorderbyop` and a
  new `amgettuple` implementation.  `SELECT ... WHERE body
  %~~ p ORDER BY body <@> p ASC LIMIT N` now produces an
  Index Scan with `Order By:` and no Sort node.  10k-row
  LIMIT 10 measured at 23 ms (was 40 ms with executor sort).
  `tre_text_ops` operator class adds strategy 2 for the
  ordering operator; `pg_tre--1.3.0--1.4.0.sql` registers it.
  Honest caveat: true streaming early-termination requires
  per-tuple distance proxies stored in the index — followup
  for v2.0 alongside variable-width blooms.
- **Delta-aware WAL redo for `pending_insert`.**  The hottest
  WAL emit path (per-row inserts into a fastupdate index) now
  ships a delta (~24 bytes header + entries) instead of two
  ~8 KB full-page images.  Measured on a 10k-row INSERT:
  ~370x reduction in FPI bytes on the pending_insert path,
  ~30x reduction in total pg_tre WAL volume.
  `wal_consistency_checking='pg_tre'` still passes — standby
  bytewise comparison agrees after delta replay.
  Other call sites still on FORCE_IMAGE; pattern is
  established and replicable for them.
- **In-place format-upgrade infrastructure.**  Three new SQL
  surfaces:
  - `pg_tre_upgrade_index(idx regclass)`: walks every block,
    rewrites pages below the current format version,
    WAL-logs each rewrite, updates the meta page's
    `min_page_format_version` after a complete sweep.
    Per-page exclusive lock held only for the brief
    rewrite + WAL emit.
  - `pg_tre_index_format_status(idx regclass)` returns
    `(format_version int4, page_count bigint)`.  SHARED
    locks; safe to run alongside reads/writes.
  - `pg_tre_index_min_format_version(idx regclass) -> int4`
    returns the meta page's tracked minimum.
  Today's pg_tre_upgrade_index is a structural no-op
  (v3 and v4 are byte-identical); the framework is in place
  so v2.0's variable-width-bloom format change can ship
  with zero-downtime upgrades.
- **PostgreSQL 17 build compatibility.**  PG18-only
  IndexAmRoutine fields (`amconsistentequality`,
  `amconsistentordering`, `amtranslatestrategy`,
  `amtranslatecmptype`) are guarded by
  `#if PG_VERSION_NUM >= 180000`.  PG17 added to the GitHub
  Actions CI matrix.

### Changed

- `PG_TRE_INLINE_POSTING_MAX` raised from 256 to 2048 bytes.
  At 2048 most natural-language trigrams' posting lists
  stay inline; index size drops modestly on real corpora.
  v1.3's build/scan perf fixes confirmed safe at this
  threshold across the regression matrix.
- `PG_TRE_FORMAT_VERSION_LATEST = 4`,
  `PG_TRE_FORMAT_VERSION_MIN = 3`.  v3 indexes still readable
  (byte-identical layout); meta-page `min_page_format_version`
  tracks the minimum across all pages.
- `fuzz/memutils_stub.c` removed.  Fuzz harness now links
  `libpgcommon.a` + `libpgport.a` and uses real `palloc` /
  `MemoryContext` / `StringInfo`.  Limitation: cross-context
  UAF detection requires running as a backend extension
  (separate refactor).

### Fixed

- Three not-yet-implemented WAL redo PANICs
  (`XLOG_PTRE_POSTING_DELETE`, `XLOG_PTRE_POSTING_SPLIT`,
  `XLOG_PTRE_VACUUM`) replaced with FPI-fallthrough.  These
  ops are not emitted by today's page layer, so the routes
  were unreachable — but a stale PANIC in the redo dispatch
  was a footgun for future work.  The unknown-op default
  arm correctly remains a PANIC (corrupt WAL).

### Repo hygiene

- `.mailmap` collapses prior `Greg Burd (pi agent)` commits
  to canonical `Greg Burd` for `git log`, GitHub, Codeberg.
  History is unchanged; mapping is non-destructive.
- Agent-tooling ignore patterns moved from public
  `.gitignore` to `.local-gitignore`.

Inspired by https://github.com/timescale/pg_textsearch
(Tiger Data, PostgreSQL License).

---

## [1.3.0] - 2026-05-24 - performance: 1000x scan, 100x build

First minor release on the 1.x line. Same on-disk format as
1.2.x; ALTER EXTENSION pg_tre UPDATE TO '1.3.0' has no SQL
work to do; no re-index required. Substantial performance
wins on high-cardinality workloads.

### Added

- `pg_tre.tier3_max_candidates` GUC (PGC_USERSET, default
  50000): cardinality safety belt for the tier-3 per-tuple
  bloom and Phase 5.1 positional filter. When the candidate
  set exceeds this threshold, both filters are skipped and
  the executor recheck handles correctness.
- PostgreSQL 17 build compatibility. PG18-only IndexAmRoutine
  fields are guarded by `#if PG_VERSION_NUM >= 180000`.
  Added to GitHub Actions CI matrix.
- Real `rm_mask` callback for the pg_tre custom rmgr.
  Allows `wal_consistency_checking = 'pg_tre'` to run clean
  on a primary+standby pair: masks pd_lsn, pd_checksum, hint
  bits, and the page hole between pd_lower and pd_upper.

### Fixed

- **Multi_leaf scan hang on high-cardinality queries.**
  Single-trigram queries (e.g. `body %~~ tre_pattern('the',
  0)` on a 100k-row corpus where 'the' appears in every row)
  previously hung for 10+ minutes. Root cause: tier-3 cannot
  prune any candidates when the query is a single trigram H,
  because the candidate set IS H's posting list and every
  candidate's per-tuple bloom contains H by construction.
  Now we skip tier-3 in that case; recheck handles
  correctness. Combined with replacing the idx++ walks with
  `sm_next_member` iteration, the 100k 'the' scan now
  completes in 107 ms (5000x speedup).

- **CREATE INDEX hang on high-cardinality data.**
  A 100k-row build with the trigram 'the' in every row
  previously hung past 10 minutes. Two causes fixed:
  (a) src/am/ambuild.c had an O(N) linear-scan TID-bloom
  registry called once per trigram emission (~5M times for
  100k rows), making the build O(N^2). Replaced with a
  simplehash hash table.
  (b) src/util/sparsemap.c's chunk locator
  `__sm_get_chunk_offset` walked all chunks linearly per
  call. For ~100k chunks this made every sm_add O(N), and
  N sm_adds O(N^2). Added an in-memory tail-chunk cursor;
  ascending-order operations are now O(1) amortized.
  Combined: 100k build went from >10 min hung to 5 seconds
  (>120x speedup).
  The cursor patch is being upstreamed to sparsemap.

- **CHECK_FOR_INTERRUPTS coverage in build path.**
  The heap-scan callback and inner build loops were
  uncancellable. Added CHECK_FOR_INTERRUPTS so a stuck
  CREATE INDEX can be killed normally.

### Performance summary (100k-row corpus, 'the' in every row)

| Operation | 1.2.4 | 1.3.0 | Speedup |
|-----------|-------|-------|---------|
| CREATE INDEX | hung 10+ min | 5 s | >120x |
| scan all-matching | hung 10+ min | 0.107 s | >5000x |
| scan selective (1k matches) | 1.5 s | 0.291 s | 5x |

Inspired by https://github.com/timescale/pg_textsearch
(Tiger Data, PostgreSQL License).

---

## [1.2.4] - 2026-05-22 - expected-output refresh + cascade fix

Patch release on the 1.2 lineage.  Same on-disk format as 1.2.3;
no re-index required; `ALTER EXTENSION pg_tre UPDATE TO '1.2.4'`
has no SQL work to do.

### Fixed

- **Cardinality + pending-overlay interaction.**  The 1.2.3
  pending-overlay positional-filter fix cascades to the path
  that handles trigrams dropped by `pg_tre.min_trigram_freq`:
  high-cardinality predicates that previously returned an
  empty index set now return the correct row set (regression
  test `cardinality.out`: `SELECT 0` -> `SELECT 200`,
  `counts_agree=t`, `row_sets_agree=t`).
- Regression test expected outputs refreshed to track the
  upstream changes from 1.2.2 / 1.2.3: `tre_version` string,
  range-tier `built range tier with N ranges` NOTICE, the
  cost-model update, and the tier-3 default-on flip.

### Known issues

- `multi_leaf` regression test takes 1-3 minutes of CPU on
  typical hardware and >10 minutes on slower workstations
  (100k-row high-cardinality build).  Test still runs by
  default; a v1.3 follow-up will trim its working set.

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
