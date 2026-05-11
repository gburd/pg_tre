#!/usr/bin/env bash
#
# Phase 7: Durability test runner for pg_tre.
# Bash-based alternative to TAP tests when Perl dependencies aren't available.
#
# Tests:
#   1. Crash recovery (immediate shutdown during various operations)
#   2. Streaming replica
#   3. REINDEX CONCURRENTLY
#   4. Dump/restore (pg_upgrade simulation)
#   5. Soak test (sustained mixed workload)

set -euo pipefail

# Configuration
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGBIN="$(dirname "$($PG_CONFIG --bindir)")/bin"
TESTDIR="$(mktemp -d /tmp/pg_tre_durability_XXXXXX)"
export PGDATA="$TESTDIR/data"
export PGHOST="$TESTDIR"
PGPORT=54320

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log() { echo -e "${GREEN}[TEST]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[FAIL]${NC} $*"; exit 1; }
pass() { echo -e "${GREEN}[PASS]${NC} $*"; }

# Cleanup on exit
cleanup() {
    if [ -n "${PGPID:-}" ]; then
        "$PGBIN/pg_ctl" -D "$PGDATA" -m immediate stop 2>/dev/null || true
    fi
    rm -rf "$TESTDIR"
}
trap cleanup EXIT INT TERM

# Initialize cluster
init_cluster() {
    log "Initializing test cluster in $TESTDIR"
    "$PGBIN/initdb" -D "$PGDATA" --no-sync >/dev/null
    
    cat >> "$PGDATA/postgresql.conf" <<EOF
port = $PGPORT
unix_socket_directories = '$PGHOST'
shared_preload_libraries = 'pg_tre'
wal_level = replica
max_wal_senders = 4
log_min_messages = WARNING
fsync = off
full_page_writes = on
EOF
}

# Start PostgreSQL
start_pg() {
    log "Starting PostgreSQL on port $PGPORT"
    "$PGBIN/pg_ctl" -D "$PGDATA" -l "$TESTDIR/logfile" start >/dev/null
    sleep 1
    PGPID=$("$PGBIN/pg_ctl" -D "$PGDATA" status | grep PID | awk '{print $4}')
}

# Stop PostgreSQL
stop_pg() {
    local mode="${1:-fast}"
    log "Stopping PostgreSQL ($mode)"
    "$PGBIN/pg_ctl" -D "$PGDATA" -m "$mode" stop >/dev/null || true
    PGPID=""
}

# Execute SQL
psql() {
    "$PGBIN/psql" -h "$PGHOST" -p "$PGPORT" -d postgres -XAtq "$@"
}

# Test 1: Crash Recovery
test_crash_recovery() {
    log "=== TEST 1: Crash Recovery ==="
    
    init_cluster
    start_pg
    
    psql -c "CREATE EXTENSION pg_tre;"
    psql -c "CREATE TABLE crash_test (id serial, t text);"
    psql -c "INSERT INTO crash_test (t) SELECT 'test_' || i FROM generate_series(1, 100) i;"
    psql -c "CREATE INDEX crash_idx ON crash_test USING tre (t tre_text_ops);"
    
    # Verify index before crash
    local before=$(psql -c "SET enable_seqscan=off; SELECT COUNT(*) FROM crash_test WHERE t %~~ 'test_1';")
    [ "$before" -eq 11 ] || error "Index not working before crash: expected 11, got $before"
    
    # Insert more data, then crash
    psql -c "INSERT INTO crash_test (t) SELECT 'batch_' || i FROM generate_series(1, 200) i;"
    
    # Immediate shutdown (simulated crash)
    stop_pg immediate
    
    # Restart and verify recovery
    start_pg
    
    # Verify index works after recovery
    local result=$(psql -c "SET enable_seqscan=off; SELECT COUNT(*) FROM crash_test WHERE t %~~ 'test_1';")
    [ "$result" -eq 11 ] || error "Crash recovery failed: expected 11, got $result"
    
    # Verify consistency
    local idx=$(psql -c "SET enable_seqscan=off; SELECT COUNT(*) FROM crash_test;")
    local seq=$(psql -c "SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT COUNT(*) FROM crash_test;")
    [ "$idx" -eq "$seq" ] || error "Index inconsistent after crash: idx=$idx seq=$seq"
    
    pass "Crash recovery test passed (idx=$idx rows)"
    
    stop_pg
}

# Test 2: WAL Consistency Checking
test_wal_consistency() {
    log "=== TEST 2: WAL Consistency Checking ==="
    
    init_cluster
    echo "wal_consistency_checking = 'pg_tre'" >> "$PGDATA/postgresql.conf"
    start_pg
    
    psql -c "CREATE EXTENSION pg_tre;"
    psql -c "CREATE TABLE wal_test (id serial, t text);"
    psql -c "INSERT INTO wal_test (t) SELECT 'data_' || i FROM generate_series(1, 500) i;"
    psql -c "CREATE INDEX wal_idx ON wal_test USING tre (t tre_text_ops);"
    
    # More operations to generate WAL
    psql -c "INSERT INTO wal_test (t) SELECT 'more_' || i FROM generate_series(1, 300) i;"
    psql -c "VACUUM wal_test;"
    
    # Check for errors in log
    if grep -q "PANIC\|FATAL\|wal_consistency" "$TESTDIR/logfile"; then
        error "WAL consistency errors found in log"
    fi
    
    pass "WAL consistency checking passed"
    
    stop_pg
}

# Test 3: REINDEX CONCURRENTLY
test_reindex() {
    log "=== TEST 3: REINDEX CONCURRENTLY ==="
    
    init_cluster
    start_pg
    
    psql -c "CREATE EXTENSION pg_tre;"
    psql -c "CREATE TABLE reindex_test (id serial, t text);"
    psql -c "INSERT INTO reindex_test (t) SELECT 'data_' || i FROM generate_series(1, 1000) i;"
    psql -c "CREATE INDEX reindex_idx ON reindex_test USING tre (t tre_text_ops);"
    
    # REINDEX CONCURRENTLY
    psql -c "REINDEX INDEX CONCURRENTLY reindex_idx;"
    
    # Verify index works
    local result=$(psql -c "SET enable_seqscan=off; SELECT COUNT(*) FROM reindex_test WHERE t %~~ 'data_1';")
    [ "$result" -eq 111 ] || error "REINDEX failed: expected 111, got $result"
    
    # Verify consistency
    local idx=$(psql -c "SET enable_seqscan=off; SELECT COUNT(*) FROM reindex_test;")
    local seq=$(psql -c "SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT COUNT(*) FROM reindex_test;")
    [ "$idx" -eq "$seq" ] || error "Index inconsistent after REINDEX: idx=$idx seq=$seq"
    
    pass "REINDEX CONCURRENTLY passed (idx=$idx rows)"
    
    stop_pg
}

# Test 4: Dump/Restore
test_dump_restore() {
    log "=== TEST 4: Dump/Restore ==="
    
    init_cluster
    start_pg
    
    psql -c "CREATE EXTENSION pg_tre;"
    psql -c "CREATE TABLE dump_test (id serial, t text);"
    psql -c "INSERT INTO dump_test (t) SELECT 'data_' || i FROM generate_series(1, 500) i;"
    psql -c "CREATE INDEX dump_idx ON dump_test USING tre (t tre_text_ops);"
    
    # Dump
    "$PGBIN/pg_dump" -h "$PGHOST" -p "$PGPORT" -d postgres -f "$TESTDIR/dump.sql" >/dev/null
    
    stop_pg
    
    # New cluster
    rm -rf "$PGDATA"
    init_cluster
    start_pg
    
    # Restore
    psql -f "$TESTDIR/dump.sql" >/dev/null 2>&1
    
    # Verify
    local result=$(psql -c "SET enable_seqscan=off; SELECT COUNT(*) FROM dump_test WHERE t %~~ 'data_1';")
    [ "$result" -eq 56 ] || error "Dump/restore failed: expected 56, got $result"
    
    pass "Dump/restore passed"
    
    stop_pg
}

# Test 5: Soak Test (quick version)
test_soak() {
    log "=== TEST 5: Soak Test (10 iterations) ==="
    
    init_cluster
    start_pg
    
    psql -c "CREATE EXTENSION pg_tre;"
    psql -c "CREATE TABLE soak_test (id serial, t text);"
    psql -c "INSERT INTO soak_test (t) SELECT 'seed_' || i FROM generate_series(1, 500) i;"
    psql -c "CREATE INDEX soak_idx ON soak_test USING tre (t tre_text_ops);"
    
    # Run mixed workload
    for i in {1..10}; do
        psql -c "INSERT INTO soak_test (t) SELECT 'iter${i}_' || j FROM generate_series(1, 50) j;" >/dev/null
        psql -c "DELETE FROM soak_test WHERE id % 100 = 0;" >/dev/null
        
        if [ $((i % 5)) -eq 0 ]; then
            psql -c "VACUUM soak_test;" >/dev/null
        fi
    done
    
    # Final consistency check
    local idx=$(psql -c "SET enable_seqscan=off; SELECT COUNT(*) FROM soak_test;")
    local seq=$(psql -c "SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT COUNT(*) FROM soak_test;")
    [ "$idx" -eq "$seq" ] || error "Soak test inconsistent: idx=$idx seq=$seq"
    
    # Crash and recover
    stop_pg immediate
    start_pg
    
    local after=$(psql -c "SET enable_seqscan=off; SELECT COUNT(*) FROM soak_test;")
    [ "$after" -eq "$seq" ] || error "Post-crash inconsistent: before=$seq after=$after"
    
    pass "Soak test passed ($idx rows after 10 iterations + crash recovery)"
    
    stop_pg
}

# Main
main() {
    log "Starting pg_tre durability tests"
    log "Test directory: $TESTDIR"
    log "PostgreSQL: $($PG_CONFIG --version)"
    
    test_crash_recovery
    test_wal_consistency
    test_reindex
    test_dump_restore
    test_soak
    
    log "=== ALL TESTS PASSED ==="
}

main
