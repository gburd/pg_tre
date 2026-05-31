#!/usr/bin/env bash
# Build+install pg_tre against the freshly rebuilt PG, init a temp cluster, run regression.
set -uo pipefail

PREFIX=/home/gburd/ws/postgres/cf-5556/_rebuild_install
export PG_CONFIG="$PREFIX/bin/pg_config"
PGTRE=/home/gburd/ws/pg_tre
DATA=/tmp/pgtre_rebuild_data
SOCK=/tmp/pgtre_rebuild_sock

echo "=== pg_config: $($PG_CONFIG --version) ==="

cd "$PGTRE"
echo "=== clean + build pg_tre ==="
make PG_CONFIG="$PG_CONFIG" clean >/dev/null 2>&1 || true
if ! make PG_CONFIG="$PG_CONFIG" 2>&1 | tail -15; then
  echo "BUILD_FAILED"; exit 1
fi

echo "=== make install ==="
make PG_CONFIG="$PG_CONFIG" install 2>&1 | tail -3

BIN="$PREFIX/bin"
rm -rf "$DATA" "$SOCK"; mkdir -p "$SOCK"

echo "=== initdb ==="
"$BIN/initdb" -D "$DATA" -U postgres --no-locale --encoding=UTF8 >/tmp/pgtre_rebuild_initdb.log 2>&1
rc=$?
if [ $rc -ne 0 ]; then echo "INITDB_FAILED rc=$rc"; tail -5 /tmp/pgtre_rebuild_initdb.log; exit 1; fi
echo "initdb OK"

echo "=== start server ==="
"$BIN/pg_ctl" -D "$DATA" -l /tmp/pgtre_rebuild_server.log \
  -o "-k $SOCK -p 5456 -c listen_addresses=''" -w start
rc=$?
if [ $rc -ne 0 ]; then echo "START_FAILED"; tail -20 /tmp/pgtre_rebuild_server.log; exit 1; fi

export PGHOST="$SOCK"
export PGPORT=5456
export PGUSER=postgres
export DBNAME=contrib_regression

echo "=== run regression ==="
bash scripts/run-regress.sh 2>&1 | tail -60
regrc=${PIPESTATUS[0]}

echo "=== stop server ==="
"$BIN/pg_ctl" -D "$DATA" -m fast stop -w || true

echo "REGRESS_EXIT=$regrc"
exit $regrc
