#!/usr/bin/env bash
# load-and-index.sh — Load corpus into bench_tre and bench_trgm tables, build indexes
#
# Creates two identical tables with the same corpus data, builds indexes, measures build time.
# Idempotent: drops tables if they exist.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORPUS_FILE="${SCRIPT_DIR}/corpus.csv"
RESULTS_DIR="${SCRIPT_DIR}/results"
BUILD_TIMES_FILE="${RESULTS_DIR}/build_times.txt"

# Ensure corpus exists
if [ ! -f "${CORPUS_FILE}" ]; then
    echo "ERROR: Corpus file not found: ${CORPUS_FILE}"
    echo "Run ./fetch-corpus.sh first"
    exit 1
fi

# Ensure results directory exists
mkdir -p "${RESULTS_DIR}"

# Database connection params
export PGHOST=localhost
export PGPORT=28818
export PGDATABASE=bench_db
export PGUSER="${USER}"
export PSQLRC=/dev/null  # Disable .psqlrc for clean output

# Find pg_config
PG_CONFIG="${PG_CONFIG:-~/.pgrx/18.3/pgrx-install/bin/pg_config}"
if [ ! -x "${PG_CONFIG}" ]; then
    echo "ERROR: pg_config not found at ${PG_CONFIG}"
    echo "Set PG_CONFIG environment variable"
    exit 1
fi

PSQL="$(dirname "${PG_CONFIG}")/psql"

# Psql helper function for clean output
psql_clean() {
    "${PSQL}" -t -A -q "$@"
}

echo "==> Setting up benchmark database"
echo "    Database: ${PGDATABASE}"
echo "    Corpus: ${CORPUS_FILE}"

# Create database if not exists
psql_clean -d postgres -c "SELECT 1" > /dev/null 2>&1 || {
    echo "ERROR: Cannot connect to PostgreSQL"
    echo "Ensure PostgreSQL is running on ${PGHOST}:${PGPORT}"
    exit 1
}

psql_clean -d postgres -c "DROP DATABASE IF EXISTS ${PGDATABASE}" 2>/dev/null || true
psql_clean -d postgres -c "CREATE DATABASE ${PGDATABASE}"

# Create extensions
psql_clean -c "CREATE EXTENSION IF NOT EXISTS pg_tre" || {
    echo "ERROR: Failed to create pg_tre extension"
    echo "Ensure shared_preload_libraries = 'pg_tre' in postgresql.conf"
    exit 1
}
psql_clean -c "CREATE EXTENSION IF NOT EXISTS pg_trgm" || {
    echo "ERROR: Failed to create pg_trgm extension"
    exit 1
}

echo "==> Creating tables"

# Drop tables if exist (idempotent)
psql_clean -c "DROP TABLE IF EXISTS bench_tre CASCADE"
psql_clean -c "DROP TABLE IF EXISTS bench_trgm CASCADE"

# Create tables
psql_clean <<SQL
CREATE TABLE bench_tre (
    id SERIAL PRIMARY KEY,
    body TEXT NOT NULL
);

CREATE TABLE bench_trgm (
    id SERIAL PRIMARY KEY,
    body TEXT NOT NULL
);
SQL

echo "==> Loading corpus (10k rows)..."

# Load data using COPY (fast bulk load)
tail -n +2 "${CORPUS_FILE}" | psql_clean -c "\\COPY bench_tre(id, body) FROM STDIN WITH (FORMAT csv, HEADER false)"
tail -n +2 "${CORPUS_FILE}" | psql_clean -c "\\COPY bench_trgm(id, body) FROM STDIN WITH (FORMAT csv, HEADER false)"

# Verify row counts
TRE_COUNT=$(psql_clean -c "SELECT COUNT(*) FROM bench_tre")
TRGM_COUNT=$(psql_clean -c "SELECT COUNT(*) FROM bench_trgm")

echo "    bench_tre: ${TRE_COUNT} rows"
echo "    bench_trgm: ${TRGM_COUNT} rows"

if [ "${TRE_COUNT}" != "10000" ] || [ "${TRGM_COUNT}" != "10000" ]; then
    echo "ERROR: Expected 10000 rows in each table"
    exit 1
fi

# Clear build times file
echo "Build Times" > "${BUILD_TIMES_FILE}"
echo "===========" >> "${BUILD_TIMES_FILE}"
echo "" >> "${BUILD_TIMES_FILE}"

echo "==> Building pg_tre index..."

# Measure build time
TRE_START=$(date +%s.%N)
psql_clean -c "CREATE INDEX bench_tre_idx ON bench_tre USING tre (body)" 2>&1 | tee -a "${BUILD_TIMES_FILE}"
TRE_END=$(date +%s.%N)
TRE_BUILD_MS=$(echo "($TRE_END - $TRE_START) * 1000" | bc)

# Measure index size
TRE_SIZE=$(psql_clean -c "SELECT pg_size_pretty(pg_relation_size('bench_tre_idx'))")
TRE_SIZE_BYTES=$(psql_clean -c "SELECT pg_relation_size('bench_tre_idx')")

echo "" >> "${BUILD_TIMES_FILE}"
echo "pg_tre index build: ${TRE_BUILD_MS} ms" >> "${BUILD_TIMES_FILE}"
echo "pg_tre index size: ${TRE_SIZE} (${TRE_SIZE_BYTES} bytes)" >> "${BUILD_TIMES_FILE}"
echo "" >> "${BUILD_TIMES_FILE}"

echo "    Build time: ${TRE_BUILD_MS} ms"
echo "    Index size: ${TRE_SIZE}"

echo "==> Building pg_trgm index..."

# Measure build time
TRGM_START=$(date +%s.%N)
psql_clean -c "CREATE INDEX bench_trgm_idx ON bench_trgm USING gin (body gin_trgm_ops)" 2>&1 | tee -a "${BUILD_TIMES_FILE}"
TRGM_END=$(date +%s.%N)
TRGM_BUILD_MS=$(echo "($TRGM_END - $TRGM_START) * 1000" | bc)

# Measure index size
TRGM_SIZE=$(psql_clean -c "SELECT pg_size_pretty(pg_relation_size('bench_trgm_idx'))")
TRGM_SIZE_BYTES=$(psql_clean -c "SELECT pg_relation_size('bench_trgm_idx')")

echo "" >> "${BUILD_TIMES_FILE}"
echo "pg_trgm index build: ${TRGM_BUILD_MS} ms" >> "${BUILD_TIMES_FILE}"
echo "pg_trgm index size: ${TRGM_SIZE} (${TRGM_SIZE_BYTES} bytes)" >> "${BUILD_TIMES_FILE}"
echo "" >> "${BUILD_TIMES_FILE}"

echo "    Build time: ${TRGM_BUILD_MS} ms"
echo "    Index size: ${TRGM_SIZE}"

# Summary
echo "==> Build complete"
echo "    Results saved to: ${BUILD_TIMES_FILE}"

# Run VACUUM ANALYZE for query planning
echo "==> Running VACUUM ANALYZE..."
psql_clean -c "VACUUM ANALYZE bench_tre"
psql_clean -c "VACUUM ANALYZE bench_trgm"

echo "==> Done. Ready for run-queries.sh"
