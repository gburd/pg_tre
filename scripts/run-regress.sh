#!/usr/bin/env bash
#
# scripts/run-regress.sh - minimal pg_regress substitute.
#
# Our nix-built pg_regress has a broken exec path for /bin/sh in
# child processes.  This driver replicates what pg_regress does:
# drop/create a throwaway database, run each test .sql through psql,
# capture output, and diff against expected.
#
# Usage: scripts/run-regress.sh [test_name ...]
#        defaults to "pg_tre".

set -euo pipefail

cd "$(dirname "$0")/.."

PG_CONFIG="${PG_CONFIG:-pg_config}"
BINDIR=$("$PG_CONFIG" --bindir)
PSQL="$BINDIR/psql"
DROPDB="$BINDIR/dropdb"
CREATEDB="$BINDIR/createdb"

DBNAME="${DBNAME:-contrib_regression}"

TESTS=("$@")
if [ ${#TESTS[@]} -eq 0 ]; then TESTS=(pg_tre scan_exact incremental p5_read planner planner_auto p6_safety utf8 tier3 dnf_resolution sparsemap_serialize multi_leaf similarity trgm_similarity like_accel word_similarity selectivity order_by concurrently cardinality upgrade vacuum_inline posting_recycle multi_level_merge run_catalog build_estimate build_dedup flush_to_run testregex); fi

mkdir -p test/results
rm -f test/results/*.out test/results/*.diff

"$DROPDB" --if-exists "$DBNAME" >/dev/null
"$CREATEDB" "$DBNAME"

fail=0
for t in "${TESTS[@]}"; do
    sql="test/sql/${t}.sql"
    expected="test/expected/${t}.out"
    actual="test/results/${t}.out"
    if [[ ! -f "$sql" ]]; then
        echo "missing test file: $sql" >&2
        fail=1; continue
    fi
    "$PSQL" -d "$DBNAME" -X -a -f "$sql" > "$actual" 2>&1 || true

    if [[ ! -f "$expected" ]]; then
        echo "no expected file -- creating from current output: $expected"
        cp "$actual" "$expected"
        echo "ok  $t (generated expected)"
        continue
    fi

    if diff -u "$expected" "$actual" > "test/results/${t}.diff"; then
        rm -f "test/results/${t}.diff"
        echo "ok  $t"
    else
        echo "FAIL $t"
        sed -n '1,200p' "test/results/${t}.diff"
        fail=1
    fi
done

exit "$fail"
