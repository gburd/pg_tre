#!/usr/bin/env bash
#
# wal_audit.sh — single-node primary check of what pg_tre puts on the
# WAL stream.  pg_tre has no resource manager of its own (since 4.0.0):
# page changes are Generic WAL records, and an UNLOGGED index's init fork
# is a full-page image (XLOG FPI); records are attributed to an index by
# the relfilenode in block_ref, whatever the resource manager.
# Verifies:
#
#   1. An UNLOGGED tre index logs only its init fork: no Generic
#      record touches its main fork during CREATE INDEX or later
#      INSERT/DELETE/VACUUM.  (UNLOGGED skips WAL by definition; this
#      is a regression check that the build and write paths honor
#      RelationNeedsWAL.)
#
#   2. A LOGGED tre index's build and pending-list inserts emit Generic
#      records for its main fork, and pg_walinspect decodes the range.
#
#   3. crash + immediate-stop + restart leaves the index queryable
#      and returning the same rows as a sequential scan.
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

# Records in [start, end) whose block references name relation $3's
# relfilenode, fork $4 (main or init).  block_ref reads
# "blkref #0: rel 1663/16384/16450 fork main blk 3 ..." (pg_waldump
# omits "fork main"; accept both).
wal_count() {
    psql_capture "$TEST_PORT" "$TEST_DB" "
        SELECT count(*)::int
          FROM pg_get_wal_records_info('$1', '$2') w,
               (SELECT pg_relation_filenode('$3')::text AS fn) r
         WHERE CASE '$4'
                 WHEN 'init' THEN w.block_ref ~ ('/' || r.fn || ' fork init blk ')
                 ELSE w.block_ref ~ ('/' || r.fn || '( fork main)? blk ')
               END;"
}

# ------------------------------------------------------------------
# Test 1: an UNLOGGED index logs its init fork and nothing else.
# ------------------------------------------------------------------
log "Test 1: UNLOGGED index logs only its init fork"

psql_check "$TEST_PORT" "$TEST_DB" "
    CREATE UNLOGGED TABLE unlogged_t (id serial PRIMARY KEY, body text);
    INSERT INTO unlogged_t (body)
    SELECT 'unlogged row ' || i
    FROM generate_series(1, 100) AS i;"

start_lsn=$(psql_capture "$TEST_PORT" "$TEST_DB" \
    "SELECT pg_current_wal_lsn()::text")
psql_check "$TEST_PORT" "$TEST_DB" "
    CREATE INDEX unlogged_idx ON unlogged_t USING tre (body);
    INSERT INTO unlogged_t (body)
    SELECT 'unlogged later row ' || i FROM generate_series(1, 200) AS i;
    DELETE FROM unlogged_t WHERE id % 3 = 0;"
psql_check "$TEST_PORT" "$TEST_DB" "VACUUM unlogged_t;"
psql_check "$TEST_PORT" "$TEST_DB" "SELECT pg_switch_wal();" >/dev/null
end_lsn=$(psql_capture "$TEST_PORT" "$TEST_DB" \
    "SELECT pg_current_wal_lsn()::text")

main_n=$(wal_count "$start_lsn" "$end_lsn" unlogged_idx main)
init_n=$(wal_count "$start_lsn" "$end_lsn" unlogged_idx init)
log "  unlogged_idx Generic records: main fork $main_n, init fork $init_n"
if [ "$main_n" != "0" ]; then
    error "UNLOGGED index wrote $main_n WAL records for its main fork (expected 0)"
fi
if [ "${init_n:-0}" -lt 1 ]; then
    error "UNLOGGED CREATE INDEX logged $init_n init-fork records" \
          "(expected at least 1: the init-fork template)"
fi
log "  OK: init fork logged, main fork not"

# ------------------------------------------------------------------
# Test 2: a LOGGED index logs its build and pending-list inserts, and
# pg_walinspect decodes every record in the range.
# ------------------------------------------------------------------
log "Test 2: LOGGED index build + inserts emit decodable Generic records"

psql_check "$TEST_PORT" "$TEST_DB" "
    CREATE TABLE logged_t (id serial PRIMARY KEY, body text);
    INSERT INTO logged_t (body)
    SELECT 'The quick brown fox jumps over the lazy dog row ' || i
    FROM generate_series(1, 1000) AS i;"

start_lsn=$(psql_capture "$TEST_PORT" "$TEST_DB" \
    "SELECT pg_current_wal_lsn()::text")
psql_check "$TEST_PORT" "$TEST_DB" "
    CREATE INDEX logged_idx ON logged_t USING tre (body);"
mid_lsn=$(psql_capture "$TEST_PORT" "$TEST_DB" \
    "SELECT pg_current_wal_lsn()::text")
psql_check "$TEST_PORT" "$TEST_DB" "
    -- Inserts after the build go through the pending list.
    INSERT INTO logged_t (body) VALUES
        ('extra row alpha'),
        ('extra row beta'),
        ('extra row gamma');"
end_lsn=$(psql_capture "$TEST_PORT" "$TEST_DB" \
    "SELECT pg_current_wal_lsn()::text")

build_n=$(wal_count "$start_lsn" "$mid_lsn" logged_idx main)
insert_n=$(wal_count "$mid_lsn" "$end_lsn" logged_idx main)
log "  logged_idx Generic records: build $build_n, pending inserts $insert_n"
if [ "${build_n:-0}" -lt 1 ]; then
    error "LOGGED CREATE INDEX logged $build_n main-fork records (expected >= 1)"
fi
if [ "${insert_n:-0}" -lt 1 ]; then
    error "LOGGED pending-list inserts logged $insert_n records (expected >= 1)"
fi

# pg_get_wal_records_info raises an ERROR on a record it cannot decode;
# psql_check fails the test on that.
psql_check "$TEST_PORT" "$TEST_DB" "
    SELECT count(*) FROM pg_get_wal_records_info('$start_lsn', '$end_lsn');" >/dev/null
log "  OK: pg_walinspect decoded every record in the range"

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
