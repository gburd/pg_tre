#!/usr/bin/env bash
# bench/stress/stress-suite.sh — pg_tre at-scale adverse-conditions suite.
# Runs ON the provisioned NVMe instance (see STRESS-PLAN.md).  Builds an
# ephemeral cluster on /mnt/nvme, loads a large corpus, and runs selectable
# adverse-condition scenarios A..J, each with an accuracy oracle where it
# returns rows.  Raw CSV + SUMMARY.md land in $RESULTS.
#
# Usage (on the instance):
#   ./stress-suite.sh build-stack --src ~/pg_tre        # build PG-tre .so
#   ./stress-suite.sh init                              # cluster on /mnt/nvme
#   ./stress-suite.sh load  --rows 10000000 --shape medium
#   ./stress-suite.sh run   --only A,B,C,F,H,I,J
#   ./stress-suite.sh teardown                          # stop cluster
#
# Env: NVME (/mnt/nvme)  PGBIN ($HOME/pg18/bin)  SHARED_BUFFERS (4GB)
#      MWM (maintenance_work_mem, 1GB)  RUNS (5)  PORT (55432)
set -uo pipefail

NVME="${NVME:-/mnt/nvme}"
PGBIN="${PGBIN:-$HOME/pg18/bin}"
PGDATA="${PGDATA:-$NVME/pgdata}"
TEMPTS="${TEMPTS:-$NVME/pgtemp}"        # temp tablespace dir (build spill)
SOCK="${SOCK:-$NVME/sock}"
PORT="${PORT:-55432}"
SHARED_BUFFERS="${SHARED_BUFFERS:-4GB}"
MWM="${MWM:-1GB}"
RUNS="${RUNS:-5}"
RESULTS="${RESULTS:-$HOME/stress-results/$(hostname -s)-$(date -u +%Y%m%dT%H%M%SZ)}"
SRC="${SRC:-$HOME/pg_tre}"

export PATH="$PGBIN:$PATH"
PSQL="psql -X -q -A -t -h $SOCK -p $PORT -U postgres -d postgres"
mkdir -p "$RESULTS"
log(){ printf '[stress] %s\n' "$*" >&2; }
csv(){ printf '%s\n' "$*" >> "$RESULTS/$1"; }
sm(){  printf '%s\n' "$*" | tee -a "$RESULTS/SUMMARY.md" >&2; }

# ---------- stack build (PG assumed prebuilt at $PGBIN; build pg_tre) ----------
build_stack() {
    local src="$SRC"
    while [ $# -gt 0 ]; do case "$1" in --src) src="$2"; shift 2;; *) shift;; esac; done
    [ -x "$PGBIN/pg_config" ] || { log "PG not found at $PGBIN — build/install PG 18 first"; exit 1; }
    log "building pg_tre in $src"
    ( cd "$src" || exit 1
      # vendored TRE: apply the progress-hook patch (fair matching!) then
      # build libtre manually (autotools on AL2023 is flaky — see README).
      if ! grep -q tre_progress_check vendor/tre/lib/tre-match-parallel.c 2>/dev/null; then
          ( cd vendor/tre && ./utils/autogen.sh >/tmp/ag.log 2>&1 || true
            cp /usr/share/libtool/build-aux/ltmain.sh utils/ 2>/dev/null || true
            cp /usr/share/automake*/install-sh utils/ 2>/dev/null || true
            autoreconf -fi --warnings=none >/tmp/ar.log 2>&1 || true
            automake --add-missing --copy >/dev/null 2>&1 || true )
          git -C vendor/tre apply "$src/patches/tre-progress-hook.patch" 2>/dev/null \
            || patch -d vendor/tre -p1 < patches/tre-progress-hook.patch 2>/dev/null || true
      fi
      ( cd vendor/tre && ./configure --enable-static --disable-shared --disable-nls \
            --without-alloca CFLAGS="-fPIC -O2" >/tmp/trecfg.log 2>&1 )
      ( cd vendor/tre/lib && rm -f ./*.o && gcc -fPIC -O2 -DHAVE_CONFIG_H -I. -I.. -I../local_includes \
            -c regcomp.c regerror.c regexec.c tre-ast.c tre-compile.c tre-filter.c \
            tre-match-approx.c tre-match-backtrack.c tre-match-parallel.c tre-mem.c \
            tre-parse.c tre-stack.c xmalloc.c && mkdir -p .libs && ar rcs .libs/libtre.a ./*.o && ranlib .libs/libtre.a )
      make PG_CONFIG="$PGBIN/pg_config" vendor/lime/lime >/tmp/lime.log 2>&1 || {
          rm -f vendor/lime/lime; make PG_CONFIG="$PGBIN/pg_config" vendor/lime/lime >/tmp/lime.log 2>&1; }
      make PG_CONFIG="$PGBIN/pg_config" src/query/tre_grammar.c >/tmp/gram.log 2>&1 || true
      touch vendor/tre/lib/.libs/libtre.a
      make PG_CONFIG="$PGBIN/pg_config" >/tmp/pgtre_build.log 2>&1 || { tail -20 /tmp/pgtre_build.log; exit 1; }
      make PG_CONFIG="$PGBIN/pg_config" install >/tmp/pgtre_inst.log 2>&1
    ) || { log "pg_tre build failed"; exit 1; }
    log "pg_tre built + installed"
}

pg_running(){ "$PGBIN/pg_ctl" -D "$PGDATA" status >/dev/null 2>&1; }
start_pg(){ "$PGBIN/pg_ctl" -D "$PGDATA" -l "$NVME/pg.log" -w start >/dev/null 2>&1; }
stop_pg(){  "$PGBIN/pg_ctl" -D "$PGDATA" -m fast stop -w >/dev/null 2>&1 || true; }

init() {
    stop_pg; rm -rf "$PGDATA" "$SOCK" "$TEMPTS"; mkdir -p "$SOCK" "$TEMPTS"
    "$PGBIN/initdb" -D "$PGDATA" -U postgres --auth-local=trust --no-locale --encoding=UTF8 >/tmp/initdb.log 2>&1
    cat >> "$PGDATA/postgresql.conf" <<CONF
port = $PORT
unix_socket_directories = '$SOCK'
listen_addresses = ''
shared_preload_libraries = 'pg_tre'
shared_buffers = $SHARED_BUFFERS
maintenance_work_mem = $MWM
max_parallel_maintenance_workers = 8
max_parallel_workers = 16
max_worker_processes = 20
work_mem = 64MB
max_wal_size = 16GB
checkpoint_timeout = 30min
temp_tablespaces = 'nvme_temp'
fsync = on
CONF
    start_pg
    $PSQL -c "CREATE TABLESPACE nvme_temp LOCATION '$TEMPTS';" 2>/dev/null || true
    $PSQL -c "ALTER SYSTEM SET temp_tablespaces = 'nvme_temp';" >/dev/null 2>&1 || true
    $PSQL -c "SELECT pg_reload_conf();" >/dev/null
    $PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_tre;" -c "CREATE EXTENSION IF NOT EXISTS pg_trgm;" >/dev/null
    log "cluster up on $PGDATA (sb=$SHARED_BUFFERS mwm=$MWM temp=$TEMPTS)"
    $PSQL -c "SELECT tre_version();"
}

load() {
    local rows=10000000 shape=medium
    while [ $# -gt 0 ]; do case "$1" in --rows) rows="$2"; shift 2;; --shape) shape="$2"; shift 2;; *) shift;; esac; done
    printf '%s %s\n' "$rows" "$shape" > "$NVME/.corpus_params"   # for reload_corpus
    log "loading $rows rows shape=$shape (streaming)"
    $PSQL -c "DROP TABLE IF EXISTS t CASCADE;" -c "CREATE TABLE t(id bigint, body text);"
    python3 "$(dirname "$0")/gen_large_corpus.py" --rows "$rows" --shape "$shape" \
      | $PSQL -c "\copy t(id,body) FROM STDIN WITH (FORMAT csv, HEADER true)"
    $PSQL -c "SELECT count(*) rows, pg_size_pretty(pg_relation_size('t')) heap FROM t;"
    csv meta.csv "rows,$rows"; csv meta.csv "shape,$shape"
    csv meta.csv "heap_bytes,$($PSQL -c "SELECT pg_relation_size('t')")"
    csv meta.csv "pg_tre_sha,$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo NA)"
    csv meta.csv "instance,$(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null || echo NA)"
    # sizing prediction
    $PSQL -c "SELECT * FROM tre_estimate_index_build('t'::regclass, 2);" 2>/dev/null | tee -a "$RESULTS/estimate.txt" || true
}

# Reload the corpus to its pristine as-loaded state.  Mutating scenarios
# (D CIC-under-writes, G VACUUM-under-churn) call this first so they don't
# pollute the selectivity that read-only scenarios depend on.
reload_corpus() {
    local rows shape
    read -r rows shape < "$NVME/.corpus_params" 2>/dev/null || { rows=10000000; shape=medium; }
    log "reloading pristine corpus ($rows rows, shape=$shape)"
    $PSQL -c "DROP TABLE IF EXISTS t CASCADE;" -c "CREATE TABLE t(id bigint, body text);" >/dev/null 2>&1
    python3 "$(dirname "$0")/gen_large_corpus.py" --rows "$rows" --shape "$shape" \
      | $PSQL -c "\copy t(id,body) FROM STDIN WITH (FORMAT csv, HEADER true)" >/dev/null 2>&1
}

# accuracy oracle: index result-set == seq-scan result-set (both directions)
oracle() {  # where -> "mismatches|hits"
    $PSQL <<SQL
SET enable_seqscan=off;
CREATE TEMP TABLE _i AS SELECT id FROM t WHERE $1;
SET enable_seqscan=on; SET enable_indexscan=off; SET enable_bitmapscan=off;
CREATE TEMP TABLE _s AS SELECT id FROM t WHERE $1;
RESET enable_indexscan; RESET enable_bitmapscan;
SELECT (SELECT count(*) FROM (SELECT id FROM _i EXCEPT SELECT id FROM _s) a)
     + (SELECT count(*) FROM (SELECT id FROM _s EXCEPT SELECT id FROM _i) b)
   ||'|'|| (SELECT count(*) FROM _s);
DROP TABLE _i; DROP TABLE _s;
SQL
}

lat() {  # where seqscan -> p50 (ms)
    local w="$1" s="$2" t=""
    for _ in $(seq 1 "$RUNS"); do
        local ms; ms=$($PSQL -c "SET enable_seqscan=$s; EXPLAIN (ANALYZE,TIMING ON) SELECT id FROM t WHERE $w;" 2>/dev/null \
                 | sed -n 's/.*Execution Time: \([0-9.]*\) ms.*/\1/p')
        [ -n "$ms" ] && t="$t $ms"
    done
    printf '%s\n' $t | sort -n | awk -v n="$(printf '%s\n' $t|grep -c .)" 'NR==int((n+1)/2)'
}

build_tre_index() {  # name  extra-guc  -> seconds
    local name="$1"; local start end
    $PSQL -c "DROP INDEX IF EXISTS $name;" >/dev/null 2>&1
    start=$(date +%s.%N)
    $PSQL -c "${2:-}CREATE INDEX $name ON t USING tre(body);" >/dev/null 2>&1
    end=$(date +%s.%N)
    awk "BEGIN{printf \"%.1f\", $end-$start}"
}

# ---------------- scenarios ----------------
scen_query_matrix() {  # shared helper: run planted-token matrix w/ oracle+lat
    sm ""; sm "### query matrix ($1)"
    sm "$(printf '%-26s %8s %10s %10s' query hits p50_ms acc)"
    local rows="q_common|body %~~ tre_pattern('government',0)|off
q_mid|body %~~ tre_pattern('electrification',0)|off
q_rare|body %~~ tre_pattern('naturalize',0)|off
q_like|body LIKE '%electrific%'|off
q_approx1|body %~~ tre_pattern('govrnment',1)|off
q_nomatch|body %~~ tre_pattern('zzqxby',0)|off
q_anchored_absent|body %~~ tre_pattern('^zzqxby',0)|off
q_anchored_present|body %~~ tre_pattern('^government',0)|off"
    printf '%s\n' "$rows" | while IFS='|' read -r qid where ss; do
        local o m h p50 a
        o=$(oracle "$where"); m=${o%|*}; h=${o#*|}
        p50=$(lat "$where" "$ss")
        a=OK; [ "${m:-x}" != 0 ] && a="MISMATCH:$m"
        sm "$(printf '%-26s %8s %10s %10s' "$qid" "$h" "${p50:-NA}" "$a")"
        csv "matrix_$1.csv" "$qid,$h,${p50:-NA},$a"
    done
}

scenario_A_tempdisk() {
    sm ""; sm "## A — temp-disk exhaustion (build cliff)"
    # cap emitted-tuple temp budget low to force a clean failure, then unlimited
    local cap_mb=64
    sm "capping pg_tre.build_max_entries_mb=$cap_mb on a full-corpus build (expect clean error)"
    local out; out=$($PSQL -c "SET pg_tre.build_max_entries_mb=$cap_mb; DROP INDEX IF EXISTS t_tre_cap; CREATE INDEX t_tre_cap ON t USING tre(body);" 2>&1)
    if echo "$out" | grep -qiE 'exceeded|PROGRAM_LIMIT|limit'; then
        sm "PASS: clean limit error — $(echo "$out" | grep -iE 'exceeded|limit' | head -1)"
        csv A.csv "capped_build,clean_error"
    else
        sm "CHECK: no clean limit error (either it fit, or a hard failure): $(echo "$out"|tail -1)"
        csv A.csv "capped_build,$(echo "$out"|tail -1)"
    fi
    $PSQL -c "SELECT pg_stat_file('base').size" >/dev/null 2>&1 || true
}

scenario_B_memstarve() {
    sm ""; sm "## B — maintenance_work_mem starvation (bounded RSS)"
    $PSQL -c "SET maintenance_work_mem='64MB'; DROP INDEX IF EXISTS t_tre;" >/dev/null 2>&1
    # sample leader RSS while building
    ( $PSQL -c "SET maintenance_work_mem='64MB'; CREATE INDEX t_tre ON t USING tre(body);" >/dev/null 2>&1 ) &
    local bpid=$! peak=0
    while kill -0 "$bpid" 2>/dev/null; do
        local rss; rss=$(ps --no-headers -o rss -C postgres 2>/dev/null | sort -n | tail -1)
        [ -n "$rss" ] && [ "$rss" -gt "$peak" ] && peak=$rss
        sleep 1
    done
    wait "$bpid"
    sm "peak single-backend RSS during 64MB-mwm build: $((peak/1024)) MB (bounded => PASS)"
    csv B.csv "peak_rss_mb,$((peak/1024))"
    scen_query_matrix "B_after_64mb_build"
}

scenario_C_coldcache() {
    sm ""; sm "## C — cold-cache queries (working set >> shared_buffers=$SHARED_BUFFERS)"
    $PSQL -c "DROP INDEX IF EXISTS t_tre; CREATE INDEX t_tre ON t USING tre(body);" >/dev/null 2>&1
    $PSQL -c "SELECT pg_size_pretty(pg_relation_size('t_tre')) idx, pg_size_pretty(pg_total_relation_size('t')) tot;"
    stop_pg; sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1 || true; start_pg
    sm "COLD:"; scen_query_matrix "C_cold"
    sm "WARM:"; scen_query_matrix "C_warm"
}

scenario_D_cic() {
    sm ""; sm "## D — CREATE INDEX CONCURRENTLY under concurrent writes"
    reload_corpus   # pristine corpus (D mutates it)
    $PSQL -c "DROP INDEX IF EXISTS t_tre;" >/dev/null 2>&1
    # background churn: insert/delete rows during the CIC
    ( for _ in $(seq 1 60); do
        $PSQL -c "INSERT INTO t SELECT (SELECT max(id)+1 FROM t), 'government churn '||md5(random()::text);" >/dev/null 2>&1
        $PSQL -c "DELETE FROM t WHERE id = (SELECT min(id) FROM t WHERE body LIKE '%churn%');" >/dev/null 2>&1
        sleep 0.5
      done ) & local wpid=$!
    $PSQL -c "CREATE INDEX CONCURRENTLY t_tre ON t USING tre(body);" 2>&1 | tail -1
    kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
    local v; v=$($PSQL -c "SELECT indisvalid FROM pg_index WHERE indexrelid='t_tre'::regclass;")
    local o; o=$(oracle "body %~~ tre_pattern('government',0)"); 
    sm "CIC valid=$v  oracle(government) mismatches=${o%|*} hits=${o#*|}"
    csv D.csv "cic_valid,$v"; csv D.csv "cic_oracle_mismatch,${o%|*}"
}

scenario_E_cancel() {
    sm ""; sm "## E — cancellation mid-build (honored < 2s, no orphan)"
    $PSQL -c "DROP INDEX IF EXISTS t_tre;" >/dev/null 2>&1
    ( $PSQL -c "CREATE INDEX t_tre ON t USING tre(body);" >/dev/null 2>&1 ) & local bpid=$!
    sleep 3
    local pid; pid=$($PSQL -c "SELECT pid FROM pg_stat_activity WHERE query LIKE 'CREATE INDEX t_tre%' AND pid<>pg_backend_pid() LIMIT 1;")
    local t0 t1
    t0=$(date +%s.%N)
    [ -n "$pid" ] && $PSQL -c "SELECT pg_cancel_backend($pid);" >/dev/null 2>&1
    wait "$bpid" 2>/dev/null; t1=$(date +%s.%N)
    local present; present=$($PSQL -c "SELECT count(*) FROM pg_index WHERE indexrelid='t_tre'::regclass AND indisvalid;")
    sm "cancel latency=$(awk "BEGIN{printf \"%.2f\", $t1-$t0}")s  valid_index_left=$present (expect 0)"
    csv E.csv "cancel_latency_s,$(awk "BEGIN{printf \"%.2f\", $t1-$t0}")"; csv E.csv "valid_left,$present"
}

scenario_F_crash() {
    sm ""; sm "## F — crash recovery at scale (SIGKILL mid-build)"
    $PSQL -c "DROP INDEX IF EXISTS t_tre;" >/dev/null 2>&1
    ( $PSQL -c "CREATE INDEX t_tre ON t USING tre(body);" >/dev/null 2>&1 ) &
    sleep 4
    local pm; pm=$(head -1 "$PGDATA/postmaster.pid")
    sm "SIGKILL postmaster $pm mid-build (+ all children in its process group)"
    # Kill the whole postmaster process group so no parallel-worker child
    # survives to hold the shared-memory segment (which would block restart).
    local pgid; pgid=$(ps -o pgid= -p "$pm" 2>/dev/null | tr -d ' ')
    kill -9 "$pm" 2>/dev/null
    [ -n "$pgid" ] && kill -9 -- "-$pgid" 2>/dev/null
    pkill -9 -f "postgres.*$PGDATA" 2>/dev/null
    # Wait for the shared-memory segment / children to fully clear.  Match by
    # process GROUP, not by command line: a parallel worker retitles itself to
    # "postgres: ... CREATE INDEX" and does NOT carry the $PGDATA path, so a
    # name-based pattern misses exactly the children that hold the shm segment
    # and block restart with "pre-existing shared memory block ... in use".
    local w
    for w in $(seq 1 30); do
        if [ -n "$pgid" ]; then
            pgrep -g "$pgid" >/dev/null 2>&1 || break
            kill -9 -- "-$pgid" 2>/dev/null
        else
            pgrep -f "postgres.*$PGDATA" >/dev/null 2>&1 || break
        fi
        sleep 1
    done
    rm -f "$PGDATA/postmaster.pid" 2>/dev/null
    # Restart (crash recovery); retry a few times in case SysV shm lingers.
    local rc=1
    for w in $(seq 1 10); do
        start_pg && { rc=0; break; }
        sleep 2
    done
    local rec; rec=$(grep -icE 'PANIC|corrupt' "$NVME/pg.log" 2>/dev/null || echo 0)
    local recovered; recovered=$(grep -icE 'database system is ready to accept connections' "$NVME/pg.log" 2>/dev/null || echo 0)
    local present; present=$($PSQL -c "SELECT count(*) FROM pg_index WHERE indexrelid=to_regclass('t_tre');" 2>/dev/null || echo NA)
    sm "recovery start_rc=$rc PANIC/corrupt_in_log=$rec ready_events=$recovered index_present=$present"
    # heap must be intact and queryable after recovery (the real correctness gate)
    local rows; rows=$($PSQL -c "SELECT count(*) FROM t;" 2>/dev/null || echo NA)
    sm "post-recovery heap rows queryable=$rows"
    csv F.csv "start_rc,$rc"; csv F.csv "panic_or_corrupt,$rec"; csv F.csv "heap_rows,$rows"
    # if a valid index survived, it must pass the oracle
    local valid; valid=$($PSQL -c "SELECT count(*) FROM pg_index WHERE indexrelid=to_regclass('t_tre') AND indisvalid;" 2>/dev/null||echo 0)
    if [ "${valid:-0}" = 1 ]; then
        local o; o=$(oracle "body %~~ tre_pattern('government',0)"); sm "surviving index oracle mismatches=${o%|*}"
        csv F.csv "survivor_oracle_mismatch,${o%|*}"
    else
        sm "no valid index survived (expected for mid-build crash) — clean"
    fi
}

scenario_G_vacuum() {
    sm ""; sm "## G — VACUUM under churn (steady-state, row count held ~flat)"
    reload_corpus   # pristine corpus (G mutates it heavily)
    $PSQL -c "DROP INDEX IF EXISTS t_tre; CREATE INDEX t_tre ON t USING tre(body);" >/dev/null 2>&1
    local rows0 idx0; rows0=$($PSQL -c "SELECT count(*) FROM t"); idx0=$($PSQL -c "SELECT pg_relation_size('t_tre')")
    sm "baseline: rows=$rows0 index=$(( idx0/1024/1024 )) MB"
    # Churn IN PLACE: each round delete ~1% and reinsert the SAME count, so
    # row count stays ~flat and any index growth is genuine bloat, not data.
    local batch=$(( rows0/100 )); [ "$batch" -lt 1000 ] && batch=1000
    for r in 1 2 3 4 5 6; do
        $PSQL -c "DELETE FROM t WHERE id IN (SELECT id FROM t ORDER BY id OFFSET $((r*batch)) LIMIT $batch);" >/dev/null 2>&1
        $PSQL -c "INSERT INTO t SELECT 800000000+$r*10000000+g, 'government churn '||md5(g::text) FROM generate_series(1,$batch) g;" >/dev/null 2>&1
        $PSQL -c "VACUUM t;" >/dev/null 2>&1
        local sz; sz=$($PSQL -c "SELECT pg_relation_size('t_tre')")
        sm "round $r: rows=$($PSQL -c "SELECT count(*) FROM t") index=$(( sz/1024/1024 )) MB"
        csv G.csv "round$r,$sz"
    done
    # Settle: a few extra VACUUMs to let deferred page reclaim catch up.
    $PSQL -c "VACUUM t; VACUUM t; VACUUM t;" >/dev/null 2>&1
    local idx1; idx1=$($PSQL -c "SELECT pg_relation_size('t_tre')")
    local ratio; ratio=$(awk "BEGIN{printf \"%.2f\", $idx1/$idx0}")
    sm "after churn+settle: index=$(( idx1/1024/1024 )) MB (${ratio}x baseline)"
    sm "$([ "$(awk "BEGIN{print ($idx1<=$idx0*2)}")" = 1 ] && echo 'PASS: index within 2x baseline (bounded)' || echo 'CHECK: index >2x baseline — investigate reclaim')"
    csv G.csv "baseline_bytes,$idx0"; csv G.csv "final_bytes,$idx1"; csv G.csv "ratio,$ratio"
    # correctness after churn
    local o; o=$(oracle "body %~~ tre_pattern('government',0)"); sm "post-churn oracle mismatches=${o%|*}"
    csv G.csv "oracle_mismatch,${o%|*}"
}

scenario_H_dos() {
    sm ""; sm "## H — pathological patterns bounded by DoS guards"
    $PSQL -c "DROP INDEX IF EXISTS t_tre; CREATE INDEX t_tre ON t USING tre(body);" >/dev/null 2>&1
    for pat in "compile_bomb|a{80}{80}{80}|0" "big_k|government|5" "long_literal|$(head -c 400 /dev/zero|tr '\0' a)|0"; do
        local id p k; id=${pat%%|*}; rest=${pat#*|}; p=${rest%|*}; k=${rest##*|}
        local t0 t1 out
        t0=$(date +%s.%N)
        out=$($PSQL -c "SET statement_timeout='10s'; SELECT count(*) FROM t WHERE body %~~ tre_pattern('$p',$k);" 2>&1 | tail -1)
        t1=$(date +%s.%N)
        sm "$id: $(awk "BEGIN{printf \"%.2f\", $t1-$t0}")s -> $(echo "$out"|head -c 80)"
        csv H.csv "$id,$(awk "BEGIN{printf \"%.2f\", $t1-$t0}"),$(echo "$out"|tr ',' ' '|head -c 60)"
    done
}

scenario_I_parallel() {
    sm ""; sm "## I — parallel-build saturation + serial/parallel determinism"
    local ps pp
    pp=$(build_tre_index t_tre_par "SET max_parallel_maintenance_workers=8; ")
    local hp; hp=$(oracle "body %~~ tre_pattern('government',0)"); 
    ps=$(build_tre_index t_tre_ser "SET max_parallel_maintenance_workers=0; SET pg_tre.enable_parallel_build=off; ")
    local hs; hs=$(oracle "body %~~ tre_pattern('government',0)")
    sm "parallel build=${pp}s (hits ${hp#*|})  serial build=${ps}s (hits ${hs#*|})  same_hits=$([ "${hp#*|}" = "${hs#*|}" ] && echo YES || echo NO)"
    csv I.csv "parallel_s,$pp"; csv I.csv "serial_s,$ps"
    local stuck; stuck=$(grep -c 'stuck spinlock' "$NVME/pg.log" 2>/dev/null || echo 0)
    sm "stuck-spinlock count in log: $stuck (expect 0)"; csv I.csv "stuck_spinlock,$stuck"
}

scenario_J_surf() {
    sm ""; sm "## J — SuRF at scale"
    $PSQL -c "DROP INDEX IF EXISTS t_tre; CREATE INDEX t_tre ON t USING tre(body);" >/dev/null 2>&1
    $PSQL -c "SELECT n_keys, n_nodes, n_pages, image_bytes FROM tre_surf_stats('t_tre');" | tee -a "$RESULTS/J.csv"
    local p50; p50=$(lat "body %~~ tre_pattern('^zzqxby',0)" off)
    sm "anchored-absent reject p50=${p50}ms (expect ~O(1), sub-ms regardless of rows)"
    csv J.csv "anchored_absent_p50_ms,$p50"
}

run() {
    local only="A,B,C,H,I,J,E,F,D,G"   # read-only/analysis first; mutating (D,G) + crash (F) later
    while [ $# -gt 0 ]; do case "$1" in --only) only="$2"; shift 2;; --runs) RUNS="$2"; shift 2;; *) shift;; esac; done
    pg_running || start_pg
    sm "# pg_tre stress run — $(date -u +%FT%TZ)"
    sm "host=$(hostname) pg=$($PGBIN/pg_config --version) sha=$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null||echo NA)"
    sm "scenarios: $only  runs/query: $RUNS  results: $RESULTS"
    # Run in a fixed canonical order (read-only/analysis before mutating),
    # honoring only those requested, so a mutating scenario never pollutes a
    # later read-only one regardless of --only argument order.  F (SIGKILL)
    # runs LAST: it is the only scenario that can leave the cluster down, and
    # anything after it would then see a refused socket and report empty
    # results as if it had failed.
    local _saved_ifs="$IFS" want
    IFS=','; read -r -a _req <<< "$only"; IFS="$_saved_ifs"
    want() { local x; for x in "${_req[@]}"; do [ "$x" = "$1" ] && return 0; done; return 1; }
    for s in A B C H I J E D G F; do
        want "$s" || continue
        case "$s" in
            A) scenario_A_tempdisk;; B) scenario_B_memstarve;; C) scenario_C_coldcache;;
            D) scenario_D_cic;;      E) scenario_E_cancel;;    F) scenario_F_crash;;
            G) scenario_G_vacuum;;   H) scenario_H_dos;;       I) scenario_I_parallel;;
            J) scenario_J_surf;;
        esac
    done
    sm ""; sm "=== stress run complete; results in $RESULTS ==="
}

teardown(){ stop_pg; log "cluster stopped. (NVMe data persists until instance terminate.)"; }

cmd="${1:-}"; shift 2>/dev/null || true
case "$cmd" in
    build-stack) build_stack "$@" ;;
    init)        init "$@" ;;
    load)        load "$@" ;;
    run)         run "$@" ;;
    teardown)    teardown ;;
    *) echo "usage: $0 {build-stack|init|load|run|teardown} [opts]  (see STRESS-PLAN.md)" >&2; exit 2 ;;
esac
