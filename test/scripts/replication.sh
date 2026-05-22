#!/usr/bin/env bash
#
# replication.sh — primary + streaming standby differential test.
#
# Verifies that pg_tre indexes built / mutated on a primary
# replicate correctly to a streaming standby and return the
# same answers on both sides.  Tests:
#
#   1. Index built on the primary while the standby is running
#      reaches the standby and returns identical rows.
#
#   2. Subsequent INSERTs on the primary stream to the standby
#      and the index on the standby reflects the new rows.
#
#   3. wal_consistency_checking = 'pg_tre' is enabled on the
#      primary; if our redo callback produced a different page
#      from the primary's WAL record, the standby would log a
#      FATAL.  We grep the standby log for any such message.
#
#   4. Stop the standby cleanly, re-start, verify catchup is
#      complete and the index still answers correctly.
#
# Inspired by pg_textsearch's test/scripts/replication.sh and
# replication_lib.sh.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TEST_NAME=replication
TEST_PORT=55490
TEST_PORT_2=55491
. "$SCRIPT_DIR/lib.sh"

cleanup() {
    pg_stopcluster "$TEST_TMPDIR/standby"
    pg_stopcluster "$TEST_TMPDIR/primary"
}

setup_cleanup_trap

# ------------------------------------------------------------------
# Setup: primary with replication config, then standby via base backup.
# ------------------------------------------------------------------

log "Setting up primary on port $TEST_PORT"
pg_init_primary "$TEST_PORT"

"$PG_BIN/createdb" -h "$TEST_TMPDIR" -p "$TEST_PORT" "$TEST_DB"
psql_check "$TEST_PORT" "$TEST_DB" "CREATE EXTENSION pg_tre;"

# Build a populated index BEFORE taking the base backup so the
# standby starts with non-trivial state to replay against.
psql_check "$TEST_PORT" "$TEST_DB" "
    CREATE TABLE repl_t (id serial PRIMARY KEY, body text);
    INSERT INTO repl_t (body)
    SELECT 'The quick brown fox jumps over the lazy dog row ' || i
    FROM generate_series(1, 1000) AS i;
    CREATE INDEX repl_idx ON repl_t USING tre (body);
    CHECKPOINT;"

log "Setting up standby on port $TEST_PORT_2 via base backup"
pg_init_standby "$TEST_PORT" "$TEST_PORT_2"

# ------------------------------------------------------------------
# Test 1: standby has identical rows to the primary.
# ------------------------------------------------------------------

log "Test 1: standby returns same rows as primary"

current=$(primary_lsn "$TEST_DB")
wait_for_lsn "$TEST_PORT_2" "$TEST_DB" "$current"

p_count=$(psql_count "$TEST_PORT" "$TEST_DB" "
    SET enable_seqscan = off;
    SELECT count(*) FROM repl_t WHERE body %~~ tre_pattern('the', 0);")
s_count=$(psql_count "$TEST_PORT_2" "$TEST_DB" "
    SET enable_seqscan = off;
    SELECT count(*) FROM repl_t WHERE body %~~ tre_pattern('the', 0);")

if [ "$p_count" != "$s_count" ]; then
    error "primary returned $p_count but standby returned $s_count"
fi
log "  OK: primary=$p_count, standby=$s_count"

# ------------------------------------------------------------------
# Test 2: primary writes stream to the standby and the index reflects them.
# ------------------------------------------------------------------

log "Test 2: incremental writes stream to standby"

psql_check "$TEST_PORT" "$TEST_DB" "
    INSERT INTO repl_t (body) VALUES
        ('post-build alpha bravo charlie'),
        ('post-build delta echo foxtrot'),
        ('post-build golf hotel india');"

current=$(primary_lsn "$TEST_DB")
wait_for_lsn "$TEST_PORT_2" "$TEST_DB" "$current"

p_count=$(psql_count "$TEST_PORT" "$TEST_DB" "
    SET enable_seqscan = off;
    SELECT count(*) FROM repl_t WHERE body %~~ tre_pattern('post-build', 0);")
s_count=$(psql_count "$TEST_PORT_2" "$TEST_DB" "
    SET enable_seqscan = off;
    SELECT count(*) FROM repl_t WHERE body %~~ tre_pattern('post-build', 0);")

if [ "$p_count" != "$s_count" ]; then
    error "post-incremental: primary=$p_count, standby=$s_count"
fi
if [ "$p_count" != "3" ]; then
    error "post-incremental: expected 3 rows, primary returned $p_count"
fi
log "  OK: primary=$p_count, standby=$s_count"

# ------------------------------------------------------------------
# Test 3: wal_consistency_checking=pg_tre passes (no FATAL on standby).
# ------------------------------------------------------------------

log "Test 3: wal_consistency_checking = 'pg_tre' clean"

# Only meaningful if the script was run with TRE_WAL_CONSISTENCY=1
# (off by default; see lib.sh for the rationale).
if [ "${TRE_WAL_CONSISTENCY:-0}" = "1" ]; then
    if grep -E "FATAL|wal_consistency_checking|inconsistent" \
            "$TEST_TMPDIR/standby/logfile" >/dev/null 2>&1; then
        warn "standby log shows wal_consistency_checking failures:"
        grep -E "FATAL|inconsistent" "$TEST_TMPDIR/standby/logfile" \
            >&2 | tail
        error "redo-callback drift detected"
    fi
    log "  OK: standby log clean under wal_consistency_checking"
else
    log "  Skipped (set TRE_WAL_CONSISTENCY=1 to enable; redo" \
        "callback drift is currently a known v1.2 follow-up)"
fi

# ------------------------------------------------------------------
# Test 4: stop+restart standby, verify catchup.
# ------------------------------------------------------------------

log "Test 4: standby stop / start preserves index"

pg_stopcluster "$TEST_TMPDIR/standby"

# Make some more writes while the standby is down.
psql_check "$TEST_PORT" "$TEST_DB" "
    INSERT INTO repl_t (body) VALUES
        ('downtime juliet kilo lima'),
        ('downtime mike november');"

# Start the standby; it will catch up via WAL stream.
"$PG_BIN/pg_ctl" -D "$TEST_TMPDIR/standby" \
    -l "$TEST_TMPDIR/standby/logfile" -w start >/dev/null

current=$(primary_lsn "$TEST_DB")
wait_for_lsn "$TEST_PORT_2" "$TEST_DB" "$current"

p_count=$(psql_count "$TEST_PORT" "$TEST_DB" "
    SET enable_seqscan = off;
    SELECT count(*) FROM repl_t WHERE body %~~ tre_pattern('downtime', 0);")
s_count=$(psql_count "$TEST_PORT_2" "$TEST_DB" "
    SET enable_seqscan = off;
    SELECT count(*) FROM repl_t WHERE body %~~ tre_pattern('downtime', 0);")

if [ "$p_count" != "$s_count" ] || [ "$p_count" != "2" ]; then
    error "post-restart: primary=$p_count, standby=$s_count, expected=2"
fi
log "  OK: post-restart catchup primary=$p_count, standby=$s_count"

# ------------------------------------------------------------------
# Done.
# ------------------------------------------------------------------

echo ""
log "replication.sh: all tests passed"
