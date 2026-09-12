# pg_tre 4.0.0 qualification — results

Run on AWS **c7i.4xlarge**, Amazon Linux 2023, PostgreSQL 18.0 (`-O2`, no
cassert), pg_tre @ 4.0.0, vendored TRE at upstream master `f864ed0`.

4.0.0 replaces pg_tre's custom WAL resource manager with PostgreSQL's
generic WAL facility. The gates below are ordered by what that change can
actually break: WAL replay first, then correctness, then volume.

**Every cluster in this run was configured with NO
`shared_preload_libraries`**, which is itself part of the qualification --
the old build could not even start recovery without it.

## Verdict: QUALIFIED

### Gate 1 — WAL replay (the gate that matters for this change)

`make tap`, `wal_consistency_checking = 'all'`, no preload:

| test | result |
|------|--------|
| `tap/concurrency.pl` | **2/2** — no phantom reads under concurrent load; post-load index == seq-scan across the pattern panel |
| `tap/crash_recovery.pl` | **10/10** — two `kill -9`-mid-write cycles; every committed batch verified through both the index and a sequential scan; 397 and 756 catalog runs survived replay |
| `tap/replication.pl` | **5/5** — streaming standby matches primary on a 7-pattern panel, standby index == standby seq-scan, and a promoted replica keeps both row count and query results |
| **overall** | **`All tests successful. Result: PASS`** (17 tests) |

This gate found the release's worst bug (see Gate 5). It is slow --
crash_recovery alone runs ~55 min because far more data now survives replay
and each committed batch is verified individually -- and it is the only
thing that caught the bug. Budget for it; do not substitute the regression
suite.

### Gate 2 — regression + build

| check | result |
|-------|--------|
| regression suite (no preload) | **42/42** |
| clean build from source | **zero warnings** |
| `nix build .#pg17` / `.#pg18` | both → `pg_tre-4.0.0` |
| `nix flake check` | all checks passed |
| flake artifact, fresh cluster, no preload | `CREATE EXTENSION` + index build + query all work |
| `tre_version()` | `pg_tre 4.0.0 (TRE 0.9.0)` |

### Gate 3 — the preload requirement is actually gone

Not asserted, demonstrated. A cluster whose `postgresql.conf` contains only
the stock commented-out `#shared_preload_libraries = ''` line ran
`CREATE EXTENSION pg_tre`, built a `USING tre` index and answered queries
correctly.

For contrast, the same TAP crash-recovery test against **3.2.6** without
preload dies with:

```
FATAL:  resource manager with ID 140 not registered
```

That is the old behaviour and the reason the requirement existed: the custom
rmgr had to be registered before recovery could read pg_tre's WAL.

### Gate 4 — WAL volume (the flagged risk, which inverted)

Dropping a purpose-built delta encoder for a generic one was the conversion
plan's one performance risk. It went the other way.

20,000 rows inserted in 1,000-row batches (the pending-append path), three
runs each:

| version | WAL bytes | bytes/row |
|---------|----------:|----------:|
| 3.2.6 (custom rmgr + hand-rolled delta) | 37,274,944 / 37,249,448 / 37,259,704 | **1,863** |
| 4.0.0 (generic WAL) | 21,082,704 / 21,091,952 / 21,082,968 | **1,054** |

**43% less WAL.** Generic WAL's diff beats the bespoke encoder because the
bespoke one only special-cased the pending tail page and shipped full-page
images for everything else, whereas generic WAL diffs every registered page.

### Gate 5 — what the conversion broke, and what caught it

Four bugs, all one class: **a page written through the shared buffer instead
of the generic scratch copy.** Generic WAL diffs the registered buffer
against the scratch page, so a write to the buffer is invisible to the diff
and the record ships incomplete or empty.

| site | consequence |
|------|-------------|
| `pg_tre_run_catalog_replace` | freshly extended catalog page populated pre-registration → crash recovery would lose the entire run catalog |
| `pg_tre_run_catalog_append` | catalog header written pre-registration |
| `acquire_tail`, full-tail path | new tail init + old tail's `next_page` link written to shared buffers |
| `acquire_tail`, first-page path | `pending_head`/`pending_tail` written outside any record → **index returned ZERO rows after crash recovery** (566,050 rows inserted, index empty, sequential scan fine) |

Plus one distinct bug: the meta-page init was gated on `wal_log` with
`GenericXLogAbort` in the false branch. `GenericXLogAbort` *discards the
scratch page*, so the init was thrown away and temp/unlogged indexes got an
all-zero meta page (`meta page magic mismatch (got 0x00000000)`).

**Reading the code caught none of them.** The 42-test regression suite
passed with the worst one present. `tap/crash_recovery.pl` caught it
deterministically, 3 runs out of 3, reporting `idx=0 seq=100` for every
committed batch.

### Gate 6 — stress (1M rows)

| scenario | result |
|----------|--------|
| C — cold-cache | **0 oracle mismatches** on all 8 queries, cold and warm |
| D — CIC under concurrent writes | `indisvalid=t`, 0 mismatches |
| G — VACUUM under churn | 0 mismatches; 3.83x bloat (pre-existing, see below) |
| H — pathological patterns | compile bomb rejected in 0.01 s, high-k bounded |
| I — parallel saturation | parallel 388.5 s vs serial 500.0 s, identical hits (59,167), 0 stuck spinlocks |
| J — SuRF at scale | anchored-absent reject p50 **0.063 ms** |

## Known-unchanged, pre-existing

Scenario G's churn bloat is **3.83x, unchanged** from 3.2.x. It is real
`posting_leaf` growth at 91% occupancy from delete+reinsert churn, not
leaked pages, and is unrelated to the WAL layer. Documented in
`LIMITATIONS.md`; `REINDEX` reclaims it.

## Harness notes

- **`wal_consistency_checking = 'pg_tre'` is now a FATAL startup error.**
  Every test, script and CI workflow that set it was changed to `'all'`.
  This is not cosmetic: left in place the server will not start.
- **Bare `perl tap/foo.pl` / `prove tap/foo.pl` fails at initdb** in this
  environment (exit 255, zero tests) regardless of pg_tre version. Use
  `make tap`, which sets the paths the PostgreSQL::Test modules need. Two
  apparent "failures" during this qualification were this artifact, not the
  code -- verify with a version-independent control before believing a
  bare-prove result.
- TAP needs PostgreSQL's Perl modules, which `make install` does not place
  in the install tree: copy `src/test/perl/` from the source tree into
  `$PGPREFIX/src/test/perl` and pass `PG_TAP_PERL5LIB`.
- `make tap` needs a >70 min timeout now.
