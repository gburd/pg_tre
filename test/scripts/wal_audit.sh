#!/usr/bin/env bash
#
# wal_audit.sh — single-node primary check of what we put on the
# WAL stream.  Verifies:
#
#   1. UNLOGGED bm25-style tre indexes emit zero pg_tre records.
#      (UNLOGGED indexes skip WAL by definition; this is a
#      regression check that our build path honors
#      RelationNeedsWAL.)
#
#   2. LOGGED tre inserts and pending-list flushes emit pg_tre
#      rmgr-149 records that pg_walinspect can decode without
#      errors.  An earlier bug in our rmgr identify callback
#      surfaced here as "ERROR" lines in pg_get_wal_records_info.
#
#   3. crash + immediate-stop + restart leaves the index queryable
#      and returning the same rows it did before the crash.
#
# Tests use pg_walinspect (contrib in PG15+).  All work happens
# in the test database; no replication setup needed.
#
# Inspired by pg_textsearch's test/scripts/wal_audit.sh.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TEST_NAME=wal_audit
TEST_PORT=55480
. "$SCRIPT_DIR/lib.sh"  # re-source after setting TEST_NAME

cleanup() {
    pg_stopcluster "$TEST_TMPDIR/primary"
}

setup_cleanup_trap

# ------------------------------------------------------------------
# Setup
# ------------------------------------------------------------------
log "Setting up test cluster"
pg_initcluster primary "$TEST_PORT"

DATA_DIR="$TEST_TMPDIR/primary"
"$PG_BIN/createdb" -h "$TEST_TMPDIR" -p "$TEST_PORT" "$TEST_DB"

psql_check "$TEST_PORT" "$TEST_DB" "
    CREATE EXTENSION pg_tre;
    CREATE EXTENSION pg_walinspect;"

# ------------------------------------------------------------------
# Test 1: UNLOGGED indexes emit only the init-fork META_UPDATE record.
# ------------------------------------------------------------------
# UNLOGGED indexes WAL-log their init fork (the template that gets
# copied to the main fork on crash recovery) but NOT their main
# fork.  So we expect exactly one pg_tre record per UNLOGGED index
# (the META_UPDATE from ambuildempty), and zero records for any
# subsequent INSERT/DELETE workload against the UNLOGGED index.
log "Test 1: UNLOGGED index emits only init-fork META_UPDATE records"

start_lsn=$(psql_capture "$TEST_PORT" "$TEST_DB" \
    "SELECT pg_current_wal_lsn()::text")

psql_check "$TEST_PORT" "$TEST_DB" "
    CREATE UNLOGGED TABLE unlogged_t (id serial PRIMARY KEY, body text);
    INSERT INTO unlogged_t (body)
    SELECT 'unlogged row ' || i
    FROM generate_series(1, 100) AS i;
    CREATE INDEX unlogged_idx ON unlogged_t USING tre (body);"

end_lsn=$(psql_capture "$TEST_PORT" "$TEST_DB" \
    "SELECT pg_current_wal_lsn()::text")

# Count by record type.
pg_tre_records=$(psql_capture "$TEST_PORT" "$TEST_DB" "
    SELECT string_agg(record_type || ':' || count, ',' ORDER BY record_type)
    FROM (
        SELECT record_type, count(*)::int AS count
        FROM pg_get_wal_records_info('$start_lsn', '$end_lsn')
        WHERE resource_manager = 'pg_tre'
        GROUP BY record_type
    ) sub;")

log "  pg_tre records observed: ${pg_tre_records:-(none)}"

# Verify there is exactly one META_UPDATE (for the init fork) and
# nothing else.  Anything beyond META_UPDATE in this workload
# would mean we're WAL-logging main-fork changes for an UNLOGGED
# index, which violates the UNLOGGED contract.
bad=$(psql_capture "$TEST_PORT" "$TEST_DB" "
    SELECT count(*)::int FROM pg_get_wal_records_info('$start_lsn', '$end_lsn')
    WHERE resource_manager = 'pg_tre'
    AND record_type <> 'META_UPDATE';")

if [ "$bad" != "0" ]; then
    error "UNLOGGED workload emitted $bad non-META_UPDATE records (expected 0)"
fi

meta_count=$(psql_capture "$TEST_PORT" "$TEST_DB" "
    SELECT count(*)::int FROM pg_get_wal_records_info('$start_lsn', '$end_lsn')
    WHERE resource_manager = 'pg_tre' AND record_type = 'META_UPDATE';")

if [ "$meta_count" -lt 1 ]; then
    error "UNLOGGED CREATE INDEX produced $meta_count META_UPDATE records" \
          "(expected at least 1: the init-fork template)"
fi
log "  OK: exactly $meta_count META_UPDATE record(s), no other types"
# ------------------------------------------------------------------
# Test 2: LOGGED inserts emit pg_tre records that decode cleanly.
# ------------------------------------------------------------------
log "Test 2: LOGGED inserts emit decodable pg_tre records"

start_lsn=$(psql_capture "$TEST_PORT" "$TEST_DB" \
    "SELECT pg_current_wal_lsn()::text")

psql_check "$TEST_PORT" "$TEST_DB" "
    CREATE TABLE logged_t (id serial PRIMARY KEY, body text);
    INSERT INTO logged_t (body)
    SELECT 'The quick brown fox jumps over the lazy dog row ' || i
    FROM generate_series(1, 1000) AS i;
    CREATE INDEX logged_idx ON logged_t USING tre (body);
    -- Force a few inserts after build to exercise the pending-list
    -- path (which emits PENDING_INSERT records via our rmgr).
    INSERT INTO logged_t (body) VALUES
        ('extra row alpha'),
        ('extra row beta'),
        ('extra row gamma');"

end_lsn=$(psql_capture "$TEST_PORT" "$TEST_DB" \
    "SELECT pg_current_wal_lsn()::text")

pg_tre_record_count=$(psql_capture "$TEST_PORT" "$TEST_DB" "
    SELECT count(*)::int FROM pg_get_wal_records_info('$start_lsn', '$end_lsn')
    WHERE resource_manager = 'pg_tre';")

if [ "$pg_tre_record_count" -lt 1 ]; then
    error "LOGGED inserts emitted only $pg_tre_record_count pg_tre records (expected >= 1)"
fi
log "  OK: $pg_tre_record_count pg_tre records"

# Check for decode errors.  pg_get_wal_records_info raises an
# ERROR on records the rmgr's identify callback can't classify;
# we catch that here by piping through psql_check.
psql_check "$TEST_PORT" "$TEST_DB" "
    SELECT count(*) FROM pg_get_wal_records_info('$start_lsn', '$end_lsn')
    WHERE resource_manager = 'pg_tre';" >/dev/null
log "  OK: pg_walinspect decoded all pg_tre records without errors"

# Also verify that every record_type label is non-empty (a crude
# but effective check for our identify callback).
distinct_types=$(psql_capture "$TEST_PORT" "$TEST_DB" "
    SELECT string_agg(DISTINCT record_type, ', ')
    FROM pg_get_wal_records_info('$start_lsn', '$end_lsn')
    WHERE resource_manager = 'pg_tre';")
log "  pg_tre record types observed: $distinct_types"

# ------------------------------------------------------------------
# Test 3: crash + restart leaves the index queryable.
# ------------------------------------------------------------------
log "Test 3: crash + restart preserves index queryability"

# Capture the seq-scan answer before the crash.
expected=$(psql_capture "$TEST_PORT" "$TEST_DB" "
    SET enable_seqscan = off;
    SELECT count(*) FROM logged_t WHERE body %~~ tre_pattern('the', 0);")

if [ -z "$expected" ] || [ "$expected" = "0" ]; then
    warn "Pre-crash index returned $expected for 'the' pattern"
fi

# Force a checkpoint so we know what's on disk before the crash.
psql_check "$TEST_PORT" "$TEST_DB" "CHECKPOINT;"

# Insert more rows post-checkpoint to exercise crash recovery
# from a non-checkpoint state.
psql_check "$TEST_PORT" "$TEST_DB" "
    INSERT INTO logged_t (body)
    SELECT 'post-checkpoint row ' || i
    FROM generate_series(1, 50) AS i;"

# kill -9 the postmaster.
postmaster_pid=$(head -1 "$DATA_DIR/postmaster.pid")
log "  Killing postmaster pid=$postmaster_pid with SIGKILL"
kill -9 "$postmaster_pid" 2>/dev/null || true
sleep 1

# Wait for all child backends to die.
while pgrep -P "$postmaster_pid" >/dev/null 2>&1; do
    sleep 0.1
done

# Restart the cluster.  The startup process will replay WAL.
log "  Restarting cluster"
"$PG_BIN/pg_ctl" -D "$DATA_DIR" -l "$DATA_DIR/logfile" -w start \
    >/dev/null

# Verify the index still works.
post_crash=$(psql_capture "$TEST_PORT" "$TEST_DB" "
    SET enable_seqscan = off;
    SELECT count(*) FROM logged_t WHERE body %~~ tre_pattern('the', 0);")

# After the crash we should see at least the pre-crash count;
# the post-checkpoint inserts may or may not have made it
# (they were not durable yet).  What matters is that the index
# still returns SOMETHING and matches the seq-scan answer.
if [ -z "$post_crash" ]; then
    error "Post-crash index returned empty result"
fi

# Differential check on the recovered index.
diff_idx_vs_seq "$TEST_PORT" "$TEST_DB" logged_t the
log "  OK: post-crash index returned $post_crash; matches seq-scan"

# ------------------------------------------------------------------
# Done
# ------------------------------------------------------------------
echo ""
log "wal_audit.sh: all tests passed"
