#!/usr/bin/env bash
# ab-bench.sh — self-contained A/B benchmark for pg_tre (PG 18+).
#
# Compares, on ONE host, over an identical corpus:
#   - pg_tre @ this checkout (v2.0, all LSM/coalescing GUCs default-on)
#   - pg_tre @ v1.12.0       (last major release; the "self" baseline)
#   - pg_trgm (GIN)          (the alternative being replaced)
# with a sequential-scan ACCURACY ORACLE (index result set must equal
# seq-scan result set, per query).
#
# Requires PG >= 18 so it can use extension_control_path +
# dynamic_library_path to load pg_tre from a private $WORK dir WITHOUT
# writing to the system PostgreSQL tree (no root needed).  It builds
# both pg_tre versions from source, starts its OWN ephemeral cluster,
# loads the corpus, builds indexes, and measures build time, index
# size, query latency (p50/p95), and accuracy.  Nothing outside $WORK
# is modified.
#
# Usage:
#   ab-bench.sh --pg-config PATH --pg-bin DIR --src DIR \
#               --corpus FILE --work DIR [--old-version 1.12.0] [--runs 10]
#
#   --pg-config : pg_config (the -dev output under nix); for PGXS build.
#   --pg-bin    : dir holding postgres/initdb/psql/pg_ctl (the .out).
#                 Defaults to $(pg_config --bindir) when omitted.
#   --src       : a pg_tre checkout at HEAD (v2.0).

set -euo pipefail

PG_CONFIG="" ; PG_BIN_OVERRIDE="" ; SRC="" ; CORPUS="" ; WORK=""
OLD_VERSION="1.12.0" ; RUNS=10
MAKE="make"
REPO_URL="https://codeberg.org/gregburd/pg_tre.git"

while [ $# -gt 0 ]; do
    case "$1" in
        --pg-config)   PG_CONFIG="$2"; shift 2 ;;
        --pg-bin)      PG_BIN_OVERRIDE="$2"; shift 2 ;;
        --make)        MAKE="$2"; shift 2 ;;
        --src)         SRC="$2"; shift 2 ;;
        --corpus)      CORPUS="$2"; shift 2 ;;
        --work)        WORK="$2"; shift 2 ;;
        --old-version) OLD_VERSION="$2"; shift 2 ;;
        --runs)        RUNS="$2"; shift 2 ;;
        --repo-url)    REPO_URL="$2"; shift 2 ;;
        --extra-path)  PATH="$2:$PATH"; export PATH; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

for v in PG_CONFIG SRC CORPUS WORK; do
    [ -n "${!v}" ] || { echo "missing --${v,,}" >&2; exit 2; }
done
[ -x "$PG_CONFIG" ] || { echo "pg_config not executable: $PG_CONFIG" >&2; exit 2; }
[ -f "$CORPUS" ]    || { echo "corpus not found: $CORPUS" >&2; exit 2; }

if [ -n "$PG_BIN_OVERRIDE" ]; then PGBIN="$PG_BIN_OVERRIDE"; else PGBIN="$($PG_CONFIG --bindir)"; fi
PG_MAJOR="$($PG_CONFIG --version | awk '{print $2}' | cut -d. -f1)"
[ "$PG_MAJOR" -ge 18 ] || { echo "ab-bench requires PG >= 18 (extension_control_path); found $PG_MAJOR" >&2; exit 3; }
PSQL="$PGBIN/psql"
RESULTS="$WORK/results.txt"
HOST_S="$(hostname | cut -d. -f1)"
EXTDIR="$WORK/ext"        # private extension control/sql dir
LIBDST="$WORK/lib"        # private .so dir (dynamic_library_path)
mkdir -p "$WORK" "$EXTDIR" "$LIBDST"
: > "$RESULTS"

log()  { echo "[ab-bench] $*" >&2; }
emit() { echo "$*" | tee -a "$RESULTS" >&2; }

emit "=== pg_tre A/B benchmark ==="
emit "host:        $HOST_S ($(uname -srm))"
emit "pg version:  $($PG_CONFIG --version)"
emit "corpus:      $CORPUS ($(($(wc -l < "$CORPUS") - 1)) rows)"
emit "old version: v$OLD_VERSION"
emit "runs/query:  $RUNS"
emit "date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
emit ""

# ---- build a version into the private $EXTDIR/$LIBDST (DESTDIR stage) ----
build_and_stage() {  # label dir
    local label="$1" dir="$2"
    local stage="$WORK/stage-$label"
    log "building pg_tre ($label) in $dir"
    ( cd "$dir"
      $MAKE -s PG_CONFIG="$PG_CONFIG" clean >/dev/null 2>&1 || true
      $MAKE -s PG_CONFIG="$PG_CONFIG" 2>&1 | tail -2 ) || { echo "BUILD FAILED ($label)"; exit 1; }
    [ -d "$stage" ] && find "$stage" -mindepth 1 -delete 2>/dev/null; mkdir -p "$stage"
    ( cd "$dir" && $MAKE -s PG_CONFIG="$PG_CONFIG" DESTDIR="$stage" install >/dev/null 2>&1 ) || {
        echo "INSTALL(stage) FAILED ($label)"; exit 1; }
    # Save artifacts per label; we copy into EXTDIR/LIBDST at activation.
    mkdir -p "$WORK/art-$label"
    find "$stage" -name 'pg_tre*.so' -exec cp {} "$WORK/art-$label/" \;
    find "$stage" -name 'pg_tre.control' -exec cp {} "$WORK/art-$label/" \;
    find "$stage" -name 'pg_tre--*.sql'  -exec cp {} "$WORK/art-$label/" \;
    # control's module_pathname is '$libdir/pg_tre'; rewrite to absolute
    # so dynamic_library_path is not even needed for the SQL functions.
    sed -i.bak "s#\$libdir/pg_tre#$LIBDST/pg_tre#g" "$WORK/art-$label/pg_tre.control" 2>/dev/null || true
    # And rewrite MODULE_PATHNAME in SQL is unnecessary: PG substitutes
    # module_pathname from the control file at CREATE EXTENSION time.
}

activate() {  # label
    local label="$1"
    # PG18 appends '/extension' to each extension_control_path entry, so
    # the control + sql files live under $EXTDIR/extension/.
    mkdir -p "$EXTDIR/extension"
    rm -f "$LIBDST"/pg_tre*.so "$EXTDIR"/extension/pg_tre.control "$EXTDIR"/extension/pg_tre--*.sql
    cp "$WORK/art-$label"/pg_tre*.so       "$LIBDST/"
    cp "$WORK/art-$label"/pg_tre.control   "$EXTDIR/extension/"
    cp "$WORK/art-$label"/pg_tre--*.sql    "$EXTDIR/extension/"
}

# Install a no-op autopoint shim BEFORE any build: both the new and old
# TRE builds run autogen.sh which calls autopoint even with --disable-nls
# (its output is unused).  Provide a stub when the real one is absent.
if ! command -v autopoint >/dev/null 2>&1; then
    SHIMDIR="$WORK/shim-bin"; mkdir -p "$SHIMDIR"
    printf '#!/bin/sh\nexit 0\n' > "$SHIMDIR/autopoint"; chmod +x "$SHIMDIR/autopoint"
    export PATH="$SHIMDIR:$PATH"
    log "installed no-op autopoint shim ($SHIMDIR)"
fi

build_and_stage new "$SRC"
OLD_DIR="$WORK/pg_tre-$OLD_VERSION"
if [ ! -d "$OLD_DIR" ]; then
    log "cloning v$OLD_VERSION (recursive)"
    git clone --quiet --recurse-submodules --depth 1 --branch "v$OLD_VERSION" "$REPO_URL" "$OLD_DIR"
fi
build_and_stage old "$OLD_DIR"

# ---- ephemeral cluster ----
PGDATA="$WORK/pgdata" ; SOCK="$WORK/sock" ; PGPORT=54599
mkdir -p "$SOCK"
log "initdb"
"$PGBIN/initdb" -D "$PGDATA" --auth-local=trust -U postgres >/dev/null
cat >> "$PGDATA/postgresql.conf" <<CONF
port = $PGPORT
unix_socket_directories = '$SOCK'
listen_addresses = ''
shared_preload_libraries = '$LIBDST/pg_tre'
dynamic_library_path = '$LIBDST:\$libdir'
extension_control_path = '$EXTDIR:\$system'
shared_buffers = 256MB
maintenance_work_mem = 256MB
fsync = off
synchronous_commit = off
max_wal_size = 4GB
CONF
export PGHOST="$SOCK" PGPORT PGUSER=postgres PGDATABASE=postgres PSQLRC=/dev/null

start_pg() { "$PGBIN/pg_ctl" -D "$PGDATA" -l "$WORK/pg.log" -w start >/dev/null \
             || { echo "PG START FAILED"; tail -20 "$WORK/pg.log" >&2; exit 1; }; }
stop_pg()  { "$PGBIN/pg_ctl" -D "$PGDATA" -m fast stop >/dev/null 2>&1 || true; }
trap stop_pg EXIT

activate new
start_pg
# DB-scoped psql: pg_tre and pg_trgm both define the `%` operator, so
# they cannot share a database.  Use two: $DB_TRE (pg_tre) and
# $DB_TRGM (pg_trgm).  q() targets whatever DB is in $DB.
DB_TRE=bench_tre ; DB_TRGM=bench_trgm ; DB="$DB_TRE"
q() { "$PSQL" -X -q -t -A -F'|' -d "$DB" "$@"; }
qp() { local d="$1"; shift; "$PSQL" -X -q -t -A -F'|' -d "$d" "$@"; }

log "creating databases + loading corpus"
qp postgres -c "CREATE DATABASE $DB_TRE"  >/dev/null
qp postgres -c "CREATE DATABASE $DB_TRGM" >/dev/null

# pg_trgm DB: table t_trgm + trigram GIN.
DB="$DB_TRGM"
q -c "CREATE EXTENSION pg_trgm" >/dev/null
q -c "CREATE TABLE t_trgm (id int PRIMARY KEY, body text)" >/dev/null
tail -n +2 "$CORPUS" | q -c "\\COPY t_trgm(id, body) FROM STDIN WITH (FORMAT csv, HEADER false)"

# pg_tre DB: table t_new.
DB="$DB_TRE"
q -c "CREATE EXTENSION pg_tre" >/dev/null
q -c "CREATE TABLE t_new (id int PRIMARY KEY, body text)" >/dev/null
tail -n +2 "$CORPUS" | q -c "\\COPY t_new(id, body) FROM STDIN WITH (FORMAT csv, HEADER false)"
NROWS="$(q -c 'SELECT count(*) FROM t_new')"
emit "loaded rows: $NROWS (per table)"

build_index() {  # name table ddl   (uses current $DB)
    local name="$1" tbl="$2" ddl="$3" t0 t1 ms sz
    t0=$(date +%s.%N)
    q -c "$ddl" >/dev/null 2>>"$WORK/build.err" || { echo "INDEX BUILD FAILED: $name"; tail "$WORK/build.err" >&2; return 1; }
    t1=$(date +%s.%N)
    ms=$(awk "BEGIN{printf \"%.0f\", ($t1-$t0)*1000}")
    sz=$(q -c "SELECT pg_relation_size('${name}')")
    emit "$(printf '%-12s build %8s ms  size %12s B (%6.1f MB)' "$name" "$ms" "$sz" "$(awk "BEGIN{print $sz/1048576}")")"
}

emit ""
emit "--- index build (time + size) ---"
DB="$DB_TRE"
build_index idx_new  t_new  "CREATE INDEX idx_new ON t_new USING tre (body)"
q -c "VACUUM ANALYZE t_new"  >/dev/null
DB="$DB_TRGM"
build_index idx_trgm t_trgm "CREATE INDEX idx_trgm ON t_trgm USING gin (body gin_trgm_ops)"
q -c "VACUUM ANALYZE t_trgm" >/dev/null
DB="$DB_TRE"

# ---- query matrix: id|table|where|seqscan ----
read -r -d '' QUERIES <<'Q' || true
q1_common_government|t_new|body %~~ tre_pattern('government', 0)|off
q1_common_government|t_trgm|body ~ 'government'|off
q2_mid_electrification|t_new|body %~~ tre_pattern('electrification', 0)|off
q2_mid_electrification|t_trgm|body ~ 'electrification'|off
q3_rare_naturalize|t_new|body %~~ tre_pattern('naturalize', 0)|off
q3_rare_naturalize|t_trgm|body ~ 'naturalize'|off
q4_like_electrific|t_new|body LIKE '%electrific%'|off
q4_like_electrific|t_trgm|body LIKE '%electrific%'|off
q5_approx1_govrnment|t_new|body %~~ tre_pattern('govrnment', 1)|off
q6_nonselective_the|t_new|body %~~ tre_pattern('the', 0)|on
q6_nonselective_the|t_trgm|body ~ 'the'|on
q7_rare_nomatch|t_new|body %~~ tre_pattern('zzqxby', 0)|off
q7_rare_nomatch|t_trgm|body ~ 'zzqxby'|off
Q

# Pick the DB that owns a table (t_trgm -> pg_trgm DB, else pg_tre DB).
db_for() { case "$1" in t_trgm) echo "$DB_TRGM" ;; *) echo "$DB_TRE" ;; esac; }

accuracy() {  # table where  -> "mismatches|hits"
    local tbl="$1" where="$2" d; d="$(db_for "$tbl")"
    qp "$d" <<SQL
SET enable_seqscan=off;
CREATE TEMP TABLE _idx AS SELECT id FROM $tbl WHERE $where;
SET enable_seqscan=on; SET enable_indexscan=off; SET enable_bitmapscan=off;
CREATE TEMP TABLE _seq AS SELECT id FROM $tbl WHERE $where;
RESET enable_indexscan; RESET enable_bitmapscan;
SELECT (SELECT count(*) FROM (SELECT id FROM _idx EXCEPT SELECT id FROM _seq) a)
     + (SELECT count(*) FROM (SELECT id FROM _seq EXCEPT SELECT id FROM _idx) b),
       (SELECT count(*) FROM _seq);
DROP TABLE _idx; DROP TABLE _seq;
SQL
}

latency() {  # table where seqscan -> "p50 p95"
    local tbl="$1" where="$2" seqscan="$3" ms times sorted n med p95 d
    d="$(db_for "$tbl")"
    times=""
    for _ in $(seq 1 "$RUNS"); do
        ms=$("$PSQL" -X -q -t -A -d "$d" <<SQL 2>/dev/null | sed -n 's/.*Execution Time: \([0-9.]*\) ms.*/\1/p'
SET enable_seqscan=$seqscan;
EXPLAIN (ANALYZE, TIMING ON) SELECT id FROM $tbl WHERE $where;
SQL
)
        [ -n "$ms" ] && times="$times $ms"
    done
    sorted=$(printf '%s\n' $times | sort -n); n=$(printf '%s\n' $sorted | grep -c .)
    [ "$n" -eq 0 ] && { echo "NA NA"; return; }
    med=$(printf '%s\n' "$sorted" | awk -v n="$n" 'NR==int((n+1)/2)')
    p95=$(printf '%s\n' "$sorted" | awk -v n="$n" 'NR==int(n*0.95+0.999)')
    echo "${med:-NA} ${p95:-NA}"
}

run_matrix() {  # label
    local label="$1" qid tbl where seqscan acc_out mism hits p50 p95 acc
    emit ""
    emit "--- queries ($label): accuracy oracle + latency p50/p95 ms ---"
    printf "%-28s %8s %9s %9s %10s\n" query hits p50_ms p95_ms accuracy | tee -a "$RESULTS" >&2
    printf '%s\n' "$QUERIES" | while IFS='|' read -r qid tbl where seqscan; do
        [ -z "$qid" ] && continue
        acc_out=$(accuracy "$tbl" "$where" 2>/dev/null || echo "NA|NA")
        mism=$(echo "$acc_out" | cut -d'|' -f1); hits=$(echo "$acc_out" | cut -d'|' -f2)
        read -r p50 p95 <<<"$(latency "$tbl" "$where" "$seqscan")"
        acc="OK"; [ "${mism:-x}" != "0" ] && acc="MISMATCH:${mism}"
        printf "%-28s %8s %9s %9s %10s\n" "$qid" "${hits:-?}" "${p50:-NA}" "${p95:-NA}" "$acc" | tee -a "$RESULTS" >&2
    done
}

run_matrix "pg_tre NEW v2.0 + pg_trgm"

# ---- self baseline: pg_tre OLD v1.12.0 on the SAME data ----
emit ""
emit "--- self baseline: swap to pg_tre OLD v$OLD_VERSION, rebuild idx ---"
q -c "DROP INDEX idx_new" >/dev/null 2>&1 || true
q -c "DROP EXTENSION pg_tre CASCADE" >/dev/null 2>&1 || true
stop_pg
activate old
start_pg
q -c "CREATE EXTENSION pg_tre" >/dev/null
build_index idx_old t_new "CREATE INDEX idx_old ON t_new USING tre (body)"
q -c "VACUUM ANALYZE t_new" >/dev/null
# Only the pg_tre (t_new) queries are meaningful for the old engine.
QUERIES="$(printf '%s\n' "$QUERIES" | grep '|t_new|')"
run_matrix "pg_tre OLD v$OLD_VERSION"

emit ""
emit "=== done; full results in $RESULTS ==="
