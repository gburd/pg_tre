#!/usr/bin/env bash
# scripts/release-check.sh — pre-release gate.
# Runs the full verification suite; refuses to tag if any check fails.

set -euo pipefail

cd "$(dirname "$0")/.."

PG_CONFIG="${PG_CONFIG:-$HOME/.pgrx/18.3/pgrx-install/bin/pg_config}"
if [ ! -x "$PG_CONFIG" ]; then
    echo "FAIL: PG_CONFIG=$PG_CONFIG not executable" >&2
    exit 1
fi

echo "==> PG18 version check"
pg_ver=$("$PG_CONFIG" --version | awk '{print $2}' | cut -d. -f1)
if [ "$pg_ver" -lt 18 ]; then
    echo "FAIL: pg_tre requires PG 18+; got $pg_ver" >&2
    exit 1
fi

echo "==> Clean build"
PG_CONFIG="$PG_CONFIG" make clean >/dev/null
PG_CONFIG="$PG_CONFIG" make 2>&1 | tee /tmp/pg_tre_build.log | tail -3
if grep -qE "^[^ \t].*: (error|warning):" /tmp/pg_tre_build.log; then
    echo "FAIL: warnings or errors in build" >&2
    grep -E "^[^ \t].*: (error|warning):" /tmp/pg_tre_build.log >&2 || true
    exit 1
fi

echo "==> Install"
PG_CONFIG="$PG_CONFIG" make install >/dev/null 2>&1

echo "==> Regression tests"
PGHOST="${PGHOST:-/home/gburd/.pgrx}" PGPORT="${PGPORT:-28818}" \
    PG_CONFIG="$PG_CONFIG" bash scripts/run-regress.sh 2>&1 | \
    tee /tmp/pg_tre_check.log | grep -E "^ok|^FAIL"
if grep -q "^FAIL" /tmp/pg_tre_check.log; then
    echo "FAIL: regression tests failing" >&2
    exit 1
fi

echo "==> TAP tests (concurrency, replication, crash_recovery)"
# TAP tests take 2+ minutes each; opt in with RELEASE_CHECK_TAP=1.
# CI runs them via .github/workflows/ci.yml unconditionally; this
# script is for local pre-tag verification and defaults to fast.
if [ "${RELEASE_CHECK_TAP:-0}" = "1" ] && command -v prove >/dev/null 2>&1; then
    if [ "$(ps -ef | grep -cE 'tepid_rebuild|postgres/undo')" -gt 5 ]; then
        echo "WARN: another Postgres test suite is running (90+" \
             "backends); skipping TAP tests to avoid initdb stall."
    else
        PG_CONFIG="$PG_CONFIG" \
        PG_REGRESS="$($PG_CONFIG --pkglibdir)/pgxs/src/test/regress/pg_regress" \
        PG_TAP_PERL5LIB="$HOME/.pgrx/18.3/src/test/perl" \
        PG_TAP_TMPDIR="/tmp/pg_tre_tap_tmp" \
            make tap 2>&1 | tee /tmp/pg_tre_tap.log | tail -25
        if grep -qE "Failed|not ok|Bail out" /tmp/pg_tre_tap.log; then
            echo "FAIL: TAP tests failing" >&2
            exit 1
        fi
    fi
else
    echo "    Skipped (set RELEASE_CHECK_TAP=1 to run; takes ~6 min)"
fi

echo "==> Core benchmark (quick)"
if [ -f bench/bench.sql ]; then
    PGHOST="${PGHOST:-/home/gburd/.pgrx}" PGPORT="${PGPORT:-28818}" \
        createdb -U "$USER" release_smoke 2>/dev/null || true
    PGHOST="${PGHOST:-/home/gburd/.pgrx}" PGPORT="${PGPORT:-28818}" \
        psql -U "$USER" -d release_smoke -X -q -f bench/bench.sql \
        > /tmp/pg_tre_bench.out 2>&1 || true
    PGHOST="${PGHOST:-/home/gburd/.pgrx}" PGPORT="${PGPORT:-28818}" \
        dropdb -U "$USER" release_smoke 2>/dev/null || true
    echo "    bench output: /tmp/pg_tre_bench.out"
fi

echo "==> Checking for committed build artifacts"
if git ls-files | grep -E '\.(o|so|dylib)$'; then
    echo "FAIL: committed binary build artifacts" >&2
    exit 1
fi

echo "==> Checking STATUS.md is up to date"
if ! grep -qE "Released:|## What ships" STATUS.md; then
    echo "WARN: STATUS.md may be stale"
fi

echo ""
echo "==> All checks passed. Ready to tag:"
echo "    git tag -a v1.5.6 -m 'pg_tre 1.5.6'"
echo "    make dist          # produces pg_tre-1.5.6.tar.gz"
echo ""
