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
if grep -qE "error|warning:" /tmp/pg_tre_build.log; then
    echo "FAIL: warnings or errors in build" >&2
    grep -E "error|warning:" /tmp/pg_tre_build.log >&2 || true
    exit 1
fi

echo "==> Install"
PG_CONFIG="$PG_CONFIG" make install >/dev/null 2>&1

echo "==> Regression tests"
PG_CONFIG="$PG_CONFIG" make localcheck 2>&1 | tee /tmp/pg_tre_check.log | grep -E "^ok|^FAIL"
if grep -q "^FAIL" /tmp/pg_tre_check.log; then
    echo "FAIL: regression tests failing" >&2
    exit 1
fi

echo "==> Core benchmark (quick)"
if [ -f bench/bench.sql ]; then
    createdb -U "$USER" -h /tmp release_smoke 2>/dev/null || true
    psql -U "$USER" -h /tmp -d release_smoke -X -q -f bench/bench.sql \
        > /tmp/pg_tre_bench.out 2>&1 || true
    dropdb -U "$USER" -h /tmp release_smoke 2>/dev/null || true
    echo "    bench output: /tmp/pg_tre_bench.out"
fi

echo "==> Checking for committed build artifacts"
if git ls-files | grep -E '\.(o|so|dylib)$'; then
    echo "FAIL: committed binary build artifacts" >&2
    exit 1
fi

echo "==> Checking STATUS.md is up to date"
if ! grep -q "Phase 7\|Phase 8\|Phase 9" STATUS.md; then
    echo "WARN: STATUS.md may be stale"
fi

echo ""
echo "==> All checks passed. Ready to tag:"
echo "    git tag -s v1.0.0-rc1 -m 'pg_tre 1.0.0-rc1'"
echo "    make dist          # produces pg_tre-1.0.0-rc1.tar.gz"
echo ""
