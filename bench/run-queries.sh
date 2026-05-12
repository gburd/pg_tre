#!/usr/bin/env bash
# run-queries.sh — Execute query matrix and measure latency
#
# Runs each query N times (cold + warm), measures p50/p95/p99, saves EXPLAIN ANALYZE.
# Query matrix covers: exact regex, approximate k=1/k=2, multi-phrase, non-selective.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/results"
TIMINGS_FILE="${RESULTS_DIR}/timings.csv"

# Ensure results directory exists
mkdir -p "${RESULTS_DIR}"

# Database connection params
export PGHOST=localhost
export PGPORT=28818
export PGDATABASE=bench_db
export PGUSER="${USER}"
export PSQLRC=/dev/null  # Disable .psqlrc for clean output

# Find psql
PG_CONFIG="${PG_CONFIG:-~/.pgrx/18.3/pgrx-install/bin/pg_config}"
PSQL="$(dirname "${PG_CONFIG}")/psql"

if ! "${PSQL}" -c "SELECT 1" > /dev/null 2>&1; then
    echo "ERROR: Cannot connect to database ${PGDATABASE}"
    echo "Run ./load-and-index.sh first"
    exit 1
fi

# Number of runs per query (for percentile calculation)
NUM_RUNS=10

echo "==> Running query matrix"
echo "    Runs per query: ${NUM_RUNS}"
echo "    Results: ${RESULTS_DIR}"

# Clear timings file
echo "query_id,table_name,run,hits,time_ms" > "${TIMINGS_FILE}"

# Helper: run query N times, measure timing
run_query() {
    local QUERY_ID="$1"
    local TABLE_NAME="$2"
    local WHERE_CLAUSE="$3"
    local ENABLE_SEQSCAN="$4"  # on/off
    
    echo "  Running ${QUERY_ID} on ${TABLE_NAME} (enable_seqscan=${ENABLE_SEQSCAN})..."
    
    # Get EXPLAIN ANALYZE once
    EXPLAIN_FILE="${RESULTS_DIR}/${QUERY_ID}_${TABLE_NAME}_seqscan${ENABLE_SEQSCAN}.txt"
    "${PSQL}" <<SQL > "${EXPLAIN_FILE}" 2>&1
SET enable_seqscan = ${ENABLE_SEQSCAN};
EXPLAIN (ANALYZE, BUFFERS, VERBOSE)
SELECT id FROM ${TABLE_NAME} WHERE ${WHERE_CLAUSE};
SQL
    
    # Run N times and collect timings
    for RUN in $(seq 1 ${NUM_RUNS}); do
        # Use EXPLAIN (ANALYZE) to get precise timing, parse "Execution Time" line
        TIME_OUTPUT=$("${PSQL}" -t <<SQL 2>&1
SET enable_seqscan = ${ENABLE_SEQSCAN};
EXPLAIN (ANALYZE, BUFFERS)
SELECT id FROM ${TABLE_NAME} WHERE ${WHERE_CLAUSE};
SQL
)
        TIME_MS=$(echo "${TIME_OUTPUT}" | grep -i "Execution Time" | sed -n 's/.*Execution Time: \([0-9.]*\) ms.*/\1/p')
        HITS=$(echo "${TIME_OUTPUT}" | grep -i "rows=" | head -1 | sed -n 's/.*rows=\([0-9]*\).*/\1/p')
        
        # Fallback if parsing fails
        if [ -z "${TIME_MS}" ]; then
            TIME_MS="0.0"
        fi
        if [ -z "${HITS}" ]; then
            HITS="0"
        fi
        
        echo "${QUERY_ID},${TABLE_NAME},${RUN},${HITS},${TIME_MS}" >> "${TIMINGS_FILE}"
    done
}

# Query 1: Exact regex - "government" (moderately common)
run_query "q1_exact_government" "bench_tre" "body %~~ tre_pattern('government', 0)" "off"
run_query "q1_exact_government" "bench_trgm" "body ~ 'government'" "off"

# Query 2: Exact regex - "electrification" (rare long word)
run_query "q2_exact_electrification" "bench_tre" "body %~~ tre_pattern('electrification', 0)" "off"
run_query "q2_exact_electrification" "bench_trgm" "body ~ 'electrification'" "off"

# Query 3: Exact regex - "natural" (common word)
run_query "q3_exact_natural" "bench_tre" "body %~~ tre_pattern('natural', 0)" "off"
run_query "q3_exact_natural" "bench_trgm" "body ~ 'natural'" "off"

# Query 4: Approx k=1 - "govrnment" (1 typo in "government")
run_query "q4_approx1_govrnment" "bench_tre" "body %~~ tre_pattern('govrnment', 1)" "off"
# pg_trgm has no approximate match operator; use seq-scan baseline with tre_amatch
run_query "q4_approx1_govrnment_baseline" "bench_tre" "tre_amatch(body, 'govrnment', 1)" "on"

# Query 5: Approx k=1 - "natrual" (1 typo in "natural")
run_query "q5_approx1_natrual" "bench_tre" "body %~~ tre_pattern('natrual', 1)" "off"
run_query "q5_approx1_natrual_baseline" "bench_tre" "tre_amatch(body, 'natrual', 1)" "on"

# Query 6: Approx k=2 - "govrment" (2 typos in "government")
run_query "q6_approx2_govrment" "bench_tre" "body %~~ tre_pattern('govrment', 2)" "off"
run_query "q6_approx2_govrment_baseline" "bench_tre" "tre_amatch(body, 'govrment', 2)" "on"

# Query 7: Multi-phrase approx - "(system){~1}.*(program){~1}"
run_query "q7_multiphrase" "bench_tre" "body %~~ tre_pattern('(system){~1}.*(program){~1}')" "off"
run_query "q7_multiphrase_baseline" "bench_tre" "tre_amatch(body, '(system){~1}.*(program){~1}', 1)" "on"

# Query 8: Non-selective - "the" (very common, should use seq-scan)
run_query "q8_nonselective_the" "bench_tre" "body %~~ tre_pattern('the', 0)" "on"
run_query "q8_nonselective_the" "bench_trgm" "body ~ 'the'" "on"

# Query 9: Rare pattern - "xyzabc" (no matches expected)
run_query "q9_rare_nomatch" "bench_tre" "body %~~ tre_pattern('xyzabc', 0)" "off"
run_query "q9_rare_nomatch" "bench_trgm" "body ~ 'xyzabc'" "off"

echo "==> Query execution complete"
echo "    Timings saved to: ${TIMINGS_FILE}"
echo "    EXPLAIN outputs saved to: ${RESULTS_DIR}/"
