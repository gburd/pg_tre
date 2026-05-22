# test/scripts/lib.sh — shared helpers for pg_tre shell tests.
#
# Sourced by every test/scripts/*.sh file.  Provides:
#   - colored log/warn/error output
#   - port allocation in a private range
#   - one-shot SQL helpers via `psql_check`
#   - cluster setup/teardown with shared_preload_libraries='pg_tre'
#   - LSN-based catchup wait between primary and standby
#   - a FIFO-managed long-lived backend session for tests that
#     need the same backend across multiple primary writes
#
# Required environment (set by the sourcing script):
#   TEST_NAME       — short identifier; used in PGDATA paths
#   TEST_PORT       — port for the primary cluster
#
# Optional:
#   TEST_PORT_2     — port for a standby cluster
#   TEST_DB         — database name (defaults to $TEST_NAME)
#   PG_BIN          — directory containing pg_ctl, initdb, etc.
#                     (defaults to $(dirname $(which pg_config)))
#
# Inspired by pg_textsearch's test/scripts/replication_lib.sh.

set -euo pipefail

# --- colors --------------------------------------------------------

if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    BLUE='\033[0;34m'
    NC='\033[0m'
else
    RED=''
    GREEN=''
    YELLOW=''
    BLUE=''
    NC=''
fi

log()   { echo -e "${GREEN}[$(date +%H:%M:%S)] $*${NC}"; }
warn()  { echo -e "${YELLOW}[$(date +%H:%M:%S)] WARN: $*${NC}"; }
info()  { echo -e "${BLUE}[$(date +%H:%M:%S)] $*${NC}"; }
error() { echo -e "${RED}[$(date +%H:%M:%S)] ERROR: $*${NC}"; exit 1; }

# --- environment defaults -----------------------------------------

: "${TEST_NAME:?TEST_NAME must be set by the sourcing script}"
: "${TEST_PORT:?TEST_PORT must be set by the sourcing script}"

TEST_DB="${TEST_DB:-$TEST_NAME}"

if [ -z "${PG_BIN:-}" ]; then
    if command -v pg_config >/dev/null 2>&1; then
        PG_BIN="$(dirname "$(which pg_config)")"
    else
        error "pg_config not found and PG_BIN not set"
    fi
fi

# Workspace root resolves to the directory containing the
# Makefile.  We discover it relative to this lib file so test
# scripts can be invoked from anywhere.
LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PG_TRE_ROOT="$(cd "$LIB_DIR/../.." && pwd)"

# Per-test scratch root.  $TMPDIR is honored when set (e.g. by
# CI to point at a fast filesystem); otherwise we use /tmp.
TEST_TMPDIR_ROOT="${TMPDIR:-/tmp}"
TEST_TMPDIR="$TEST_TMPDIR_ROOT/pg_tre_$TEST_NAME"

# --- cluster lifecycle --------------------------------------------

# pg_initcluster <name> <port> [data_dir]
#
# Creates a fresh data directory, configures it with
# shared_preload_libraries='pg_tre', and starts the postmaster.
# The data directory is cluster-specific so multiple clusters can
# co-exist within a single test (primary + standby).
pg_initcluster() {
    local name=$1
    local port=$2
    local data_dir="${3:-$TEST_TMPDIR/$name}"

    [ -d "$data_dir" ] && {
        warn "Data dir $data_dir already exists; removing"
        find "$data_dir" -mindepth 1 -delete
        rmdir "$data_dir"
    }
    mkdir -p "$data_dir"

    "$PG_BIN/initdb" -D "$data_dir" \
        --auth-local=trust --auth-host=trust \
        --no-sync >/dev/null

    {
        echo "port = $port"
        echo "unix_socket_directories = '$TEST_TMPDIR'"
        echo "shared_preload_libraries = 'pg_tre'"
        echo "shared_buffers = 64MB"
        echo "max_connections = 30"
        echo "log_destination = 'stderr'"
        echo "logging_collector = off"
        echo "log_min_messages = warning"
        echo "fsync = off"
        echo "autovacuum = off"
    } >> "$data_dir/postgresql.conf"

    mkdir -p "$TEST_TMPDIR"
    "$PG_BIN/pg_ctl" -D "$data_dir" -l "$data_dir/logfile" \
        -w start >/dev/null
}

# pg_stopcluster <data_dir>
pg_stopcluster() {
    local data_dir=$1
    if [ -f "$data_dir/postmaster.pid" ]; then
        "$PG_BIN/pg_ctl" -D "$data_dir" stop -w -m fast \
            >/dev/null 2>&1 || \
        "$PG_BIN/pg_ctl" -D "$data_dir" stop -w -m immediate \
            >/dev/null 2>&1 || true
    fi
}

# psql_check <port> <db> <sql>
#
# Run a one-shot SQL command and fail loudly on error.
psql_check() {
    local port=$1 db=$2 sql=$3
    "$PG_BIN/psql" -h "$TEST_TMPDIR" -p "$port" -d "$db" -X -q \
        --set ON_ERROR_STOP=1 -c "$sql"
}

# psql_capture <port> <db> <sql>
#
# Run a one-shot SQL command and capture output (stripped of
# header/footer).  Useful for SELECT count(*).
psql_capture() {
    local port=$1 db=$2 sql=$3
    "$PG_BIN/psql" -h "$TEST_TMPDIR" -p "$port" -d "$db" -X -A -t \
        --set ON_ERROR_STOP=1 -c "$sql"
}

# wait_for_lsn <standby_port> <db> <primary_lsn>
#
# Block until the standby has replayed up to or past
# <primary_lsn>.  Loops with a 0.1s sleep until the standby's
# pg_last_wal_replay_lsn is >= primary_lsn or 30 seconds have
# passed.
wait_for_lsn() {
    local standby_port=$1 db=$2 target_lsn=$3
    local deadline=$(($(date +%s) + 30))
    while true; do
        local current
        current=$(psql_capture "$standby_port" "$db" \
            "SELECT pg_last_wal_replay_lsn()::text")
        if [ -n "$current" ] && [ "$current" != "" ]; then
            if [ "$("$PG_BIN/psql" -h "$TEST_TMPDIR" \
                    -p "$standby_port" -d "$db" -X -A -t \
                    --set ON_ERROR_STOP=1 \
                    -c "SELECT '$current'::pg_lsn >= '$target_lsn'::pg_lsn")" \
                = "t" ]; then
                return 0
            fi
        fi
        [ "$(date +%s)" -ge "$deadline" ] && \
            error "wait_for_lsn timed out at $current vs $target_lsn"
        sleep 0.1
    done
}

# create_basic_table <port> <db> <name> [n_rows]
#
# Helper: create a small table with the standard "the quick brown
# fox" rows and a tre index.  Used by stress and audit scripts.
create_basic_table() {
    local port=$1 db=$2 name=$3 n=${4:-1000}
    psql_check "$port" "$db" "
        DROP TABLE IF EXISTS $name;
        CREATE TABLE $name (id serial PRIMARY KEY, body text);
        INSERT INTO $name (body)
        SELECT 'The quick brown fox jumps over the lazy dog row ' || i
        FROM generate_series(1, $n) AS i;
        CREATE INDEX ${name}_idx ON $name USING tre (body);"
}

# diff_idx_vs_seq <port> <db> <table> <pattern>
#
# Run the same %~~ query under index scan and seq scan, compare
# the row sets, exit non-zero if they disagree.
diff_idx_vs_seq() {
    local port=$1 db=$2 table=$3 pattern=$4
    local result
    result=$(psql_capture "$port" "$db" "
        SET enable_seqscan = off;
        CREATE TEMP TABLE _idx_ids AS
            SELECT id FROM $table
            WHERE body %~~ tre_pattern('$pattern', 0);

        SET enable_seqscan = on;
        SET enable_indexscan = off;
        SET enable_bitmapscan = off;
        CREATE TEMP TABLE _seq_ids AS
            SELECT id FROM $table
            WHERE body %~~ tre_pattern('$pattern', 0);

        SELECT
            ((SELECT count(*) FROM _idx_ids) =
             (SELECT count(*) FROM _seq_ids)) AND
            NOT EXISTS (SELECT 1 FROM _idx_ids
                        EXCEPT SELECT 1 FROM _seq_ids) AND
            NOT EXISTS (SELECT 1 FROM _seq_ids
                        EXCEPT SELECT 1 FROM _idx_ids);
        DROP TABLE _idx_ids;
        DROP TABLE _seq_ids;")
    # The CREATE TEMP TABLE / SET statements emit echo lines even
    # under -A -t; take the last non-empty line as the boolean.
    local final
    final=$(printf '%s\n' "$result" | grep -E '^[tf]$' | tail -1)
    if [ "$final" != "t" ]; then
        error "differential check failed for pattern '$pattern'" \
              "on $table (returned: $final; full output: $result)"
    fi
}

# --- cleanup / trap registration ----------------------------------

# Every test script should call `setup_cleanup_trap` after
# defining a `cleanup` function.  The trap stops any clusters and
# removes the per-test scratch dir.
setup_cleanup_trap() {
    trap 'cleanup_then_exit $?' EXIT INT TERM
}

cleanup_then_exit() {
    local rc=$1
    if declare -F cleanup >/dev/null; then
        cleanup || true
    fi
    if [ -d "$TEST_TMPDIR" ]; then
        find "$TEST_TMPDIR" -mindepth 1 -delete 2>/dev/null || true
        rmdir "$TEST_TMPDIR" 2>/dev/null || true
    fi
    exit "$rc"
}
