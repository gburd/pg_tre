# pg_tre Testing Guide

This document describes how to run the pg_tre test suite, which consists of SQL regression tests and TAP (Test Anything Protocol) tests.

## Overview

pg_tre has two test suites:

1. **SQL Regression Tests** (`test/sql/*.sql`) — Functional correctness tests that run SQL queries and compare output against expected results.
2. **TAP Tests** (`tap/*.pl`) — Integration tests that exercise durability, concurrency, and replication using PostgreSQL::Test::Cluster.

## Prerequisites

### For Regression Tests

- PostgreSQL 18+ built and installed (via PG_CONFIG)
- pg_tre extension compiled and installed
- Running PostgreSQL instance with `shared_preload_libraries = 'pg_tre'`

### For TAP Tests

- All regression test prerequisites
- Perl 5.x with Test::More and Test::Harness
- PostgreSQL::Test::Cluster module (usually in `$(pg_config --libdir)/perl5`)
- `prove` command-line test runner

On most PostgreSQL installations, the TAP testing modules are included. If not, install from CPAN or your distribution's package manager.

## Running Regression Tests

### Method 1: Using the Convenience Script

```bash
PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config scripts/run-regress.sh
```

This script:
- Creates a temporary database `contrib_regression`
- Runs each test SQL file through psql
- Compares output against expected results
- Reports ok/FAIL for each test

### Method 2: Using the Makefile

```bash
PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config make localcheck
```

This invokes `scripts/run-regress.sh` via the Makefile.

### Current Regression Tests

| Test | Coverage |
|------|----------|
| `pg_tre.sql` | Extension creation, legacy UDFs, basic index creation |
| `parser.sql` | Regex parser, AST construction, tokenization |
| `scan_exact.sql` | k=0 exact regex scanning, differential tests |
| `incremental.sql` | INSERT, pending list, VACUUM, overlay scans |
| `p5_read.sql` | k>0 approximate matching, tiling, tier-3 bloom |
| `planner.sql` | Cost estimation, selectivity, plan choice |
| `planner_auto.sql` | Planner auto-tuning, metapage cardinalities |
| `p6_safety.sql` | DoS limits, pattern validation, error handling |
| `utf8.sql` | UTF-8 text handling, multibyte characters |
| `tier3.sql` | Per-tuple bloom filter validation |
| `dnf_resolution.sql` | DNF (tiled alternatives) AND/OR correctness |

Expected result: all tests report `ok`.

## Running TAP Tests

### v1.0.0-final Blocker Tests (tap/)

These are the production-ready tests that close the v1.0.0-final blockers:

```bash
PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config make tap
```

This runs:

1. **tap/concurrency.pl** — Concurrent writers + readers + vacuumer
   - 8 writer processes inserting random rows for 30 seconds
   - 4 reader processes comparing index vs seq-scan continuously
   - 1 vacuumer process running VACUUM every 5 seconds
   - Final differential check: 10 patterns, index == seq-scan
   - **Closes blocker**: Concurrency TAP test

2. **tap/replication.pl** — Streaming replication and replica promotion
   - Creates primary + streaming replica
   - Applies 100K random insert/update/delete operations
   - Waits for replica catchup
   - Verifies bit-exact result equality for 10 patterns
   - Promotes replica and re-verifies
   - **Closes blocker**: Streaming replication TAP test

3. **tap/crash_recovery.pl** — WAL replay correctness under kill -9
   - Starts continuous background writer
   - After 10 seconds, kills postmaster with -9
   - Restarts and verifies WAL replay via differential check
   - Repeats cycle 3 times to catch compounding corruption
   - **Closes blocker**: Crash-recovery-under-load TAP test

### Expected Runtime

All three TAP tests complete in **< 5 minutes** total on a developer laptop:
- `concurrency.pl`: ~35 seconds (30s load + 5s checks)
- `replication.pl`: ~90 seconds (100K ops in batches + catchup)
- `crash_recovery.pl`: ~45 seconds (3 cycles × 10s load + 5s recovery)

### Phase 7 Tests (test/t/)

The `test/t/` directory contains the earlier Phase 7 tests that are currently blocked by bugs:

```bash
PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config make tapcheck
```

These tests (`001_crash_recovery.pl`, `002_replica.pl`, etc.) were written during Phase 7 but cannot run yet due to pre-existing issues in ambuild and function exports. They are retained for historical context and will be re-enabled once those bugs are fixed.

**Do not use `make tapcheck` for v1.0.0 validation.** Use `make tap` instead.

## Full Test Suite

To run everything (regression + TAP):

```bash
PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config scripts/release-check.sh
```

This script runs:
1. Clean build with warning check
2. Installation
3. All regression tests
4. All TAP tests (tap/)
5. Quick benchmark smoke test
6. Git artifact check

Expected result: `All checks passed. Ready to tag.`

## Troubleshooting

### "prove not found"

Install Test::Harness:
```bash
cpan Test::Harness
# or on Debian/Ubuntu:
sudo apt-get install libtest-harness-perl
```

### "Can't locate PostgreSQL/Test/Cluster.pm"

The PostgreSQL TAP modules are usually installed with PostgreSQL. Check:
```bash
find $(pg_config --libdir) -name Cluster.pm
```

If missing, you may need to install PostgreSQL development packages or rebuild PostgreSQL with `--enable-tap-tests`.

### TAP test hangs

TAP tests create temporary PostgreSQL instances. If a test hangs:
1. Check available ports (tests auto-assign ports)
2. Check disk space (each test creates a temp data directory)
3. Check for orphaned postgres processes (`ps aux | grep postgres`)

### TAP test fails with "could not connect"

Ensure your firewall allows local connections and that PostgreSQL can bind to loopback interfaces.

### "Index scan != seq scan"

This indicates a correctness bug in the index. The test output will show which pattern failed and the mismatched row counts. File an issue with:
- The failing pattern
- The full test log
- Output of `git log --oneline -10`

## Test Development

### Adding a Regression Test

1. Create `test/sql/my_test.sql`
2. Run it to generate `test/expected/my_test.out`
3. Add `my_test` to `REGRESS` in the Makefile
4. Run `make localcheck` to verify

### Adding a TAP Test

1. Create `tap/my_test.pl` following the structure of existing tests
2. Use `PostgreSQL::Test::Cluster->new('my_test')` for isolation
3. Add `use Test::More; done_testing();` for proper TAP output
4. Verify with `prove -v tap/my_test.pl`

## Continuous Integration

The GitHub Actions CI matrix (`.github/workflows/ci.yml`) runs all tests on every push:
- Ubuntu 22.04 with PostgreSQL 18 from apt
- Regression tests via `make installcheck`
- TAP tests via `make tap`

## Performance Notes

- Regression tests run in **< 10 seconds** (single psql session, small fixtures)
- TAP tests run in **< 5 minutes** (parallel test instances, larger workloads)
- Use `make localcheck` for quick iteration during development
- Use `scripts/release-check.sh` for full gate before commits

## Test Coverage Summary

| Area | Regression | TAP | Total |
|------|------------|-----|-------|
| Index creation | ✓ | | 1 |
| Exact regex (k=0) | ✓ | | 1 |
| Approximate (k>0) | ✓ | | 1 |
| Incremental writes | ✓ | ✓ | 2 |
| VACUUM | ✓ | ✓ | 2 |
| Planner integration | ✓ | | 2 |
| Concurrency | | ✓ | 1 |
| Streaming replication | | ✓ | 1 |
| Crash recovery | | ✓ | 1 |
| UTF-8 | ✓ | | 1 |
| Bloom filters | ✓ | | 1 |
| DNF correctness | ✓ | | 1 |

Total: **10 regression tests**, **3 TAP tests** (v1.0.0-final blockers), **5 Phase 7 TAP tests** (blocked).

---

Last updated: 2025-05-13
Status: v1.0.0-final blocker tests COMPLETE
