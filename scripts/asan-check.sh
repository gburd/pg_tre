#!/usr/bin/env bash
# scripts/asan-check.sh -- run the pg_tre regression suite inside an
# AddressSanitizer-instrumented PostgreSQL.
#
# Why the SERVER must be instrumented, not just pg_tre: an out-of-bounds store
# performed by PostgreSQL code on pg_tre's behalf (e.g. pg_mb2wchar_with_len
# writing its terminator past a pg_tre stack variable -- the 4.2.0 similarity
# crash) happens inside postgres, so ASan on pg_tre.so alone never sees it.
# Verified: extension-only ASan was silent on that bug; this script catches it.
#
# Usage:
#   scripts/asan-check.sh                 # build (cached) + run full suite
#   scripts/asan-check.sh test1 test2     # run a subset
# Env:
#   PG_VERSION   PostgreSQL release to build          (default 18.0)
#   ASAN_PREFIX  where the ASan PG is installed/cached (default ~/pg-asan-$PG_VERSION)
#   ASAN_WORK    scratch dir for data/logs             (default /tmp/pg_tre-asan)
#   JOBS         make parallelism                      (default nproc)
#
# Exit: 0 clean; 1 regression failure; 2 sanitizer report; 3 setup failure.
set -uo pipefail

PG_VERSION=${PG_VERSION:-18.0}
ASAN_PREFIX=${ASAN_PREFIX:-$HOME/pg-asan-$PG_VERSION}
ASAN_WORK=${ASAN_WORK:-/tmp/pg_tre-asan}
JOBS=${JOBS:-$(nproc)}
SRC=$(cd "$(dirname "$0")/.." && pwd)
PORT=${ASAN_PORT:-55499}

SAN_CFLAGS="-O1 -g -fsanitize=address -fno-omit-frame-pointer"
SAN_LDFLAGS="-fsanitize=address"

# Set before ANY instrumented binary runs -- including pg_config, which the
# Makefile calls to detect the PG major: with LeakSanitizer on, pg_config's
# own at-exit leaks make it exit 1 and the build aborts.  detect_leaks=0 also
# because PostgreSQL leaks into long-lived memory contexts by design.
mkdir -p "$ASAN_WORK/reports"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:print_stacktrace=1:log_path=$ASAN_WORK/reports/asan"

log() { printf '[asan] %s\n' "$*" >&2; }
die() { log "$*"; exit 3; }

# ---------------------------------------------------------------------------
# 1. ASan-instrumented PostgreSQL (cached by prefix).
# ---------------------------------------------------------------------------
if [ ! -x "$ASAN_PREFIX/bin/postgres" ]; then
    log "building PostgreSQL $PG_VERSION with ASan into $ASAN_PREFIX"
    b=$(mktemp -d)
    curl -fsSL "https://ftp.postgresql.org/pub/source/v$PG_VERSION/postgresql-$PG_VERSION.tar.bz2" \
        | tar xj -C "$b" --strip-components=1 || die "PostgreSQL download failed"
    ( cd "$b" &&
      ./configure --prefix="$ASAN_PREFIX" --enable-debug --enable-cassert \
          --without-readline --without-zlib \
          CFLAGS="$SAN_CFLAGS" LDFLAGS="$SAN_LDFLAGS" >"$b/configure.log" 2>&1 &&
      make -j"$JOBS" >"$b/make.log" 2>&1 &&
      make install >/dev/null 2>&1 &&
      for c in pg_trgm pageinspect amcheck pgstattuple pg_freespacemap; do
          make -C contrib/$c install >/dev/null 2>&1 || exit 1
      done ) || { tail -20 "$b"/make.log "$b"/configure.log 2>/dev/null; die "PostgreSQL ASan build failed"; }
fi
PGB=$ASAN_PREFIX/bin
export PATH=$PGB:$PATH

# ---------------------------------------------------------------------------
# 2. pg_tre, instrumented the same way.  libtre.a must stay on SHLIB_LINK.
# ---------------------------------------------------------------------------
cd "$SRC"
log "building pg_tre against $PGB/pg_config"
# Rebuild the vendored TRE and Lime from source with THIS toolchain.  Objects
# and configure output left behind by another environment link or run badly
# here: a nix-built libtre.a references glibc 2.38's __isoc23_strtol (so
# pg_tre.so won't load), and a nix-configured TRE Makefile points at a
# /nix/store shell.  Removing configure (as well as config.h) forces the
# Makefile's full autogen -> configure -> make chain, which is also what a
# fresh CI checkout runs.  Needs autoconf, automake, libtool, autopoint.
make -s -C vendor/tre distclean >/dev/null 2>&1
find vendor/tre \( -name '*.o' -o -name '*.lo' -o -name '*.la' \) -delete 2>/dev/null
find vendor/tre -name 'libtre.a' -delete 2>/dev/null
find vendor/tre -maxdepth 1 \( -name config.h -o -name config.status -o -name Makefile -o -name configure \) -delete 2>/dev/null
# The patch stamp is not proof the patch is in: a copied or reset tree keeps
# the stamp and loses the patch, and make then skips it -- silently building a
# TRE without the compile/match-timeout hooks (multi_level_merge fails).
# Drop the stamp so the Makefile re-runs its idempotent apply-or-already-applied
# check, then verify the hooks really are there.
find vendor/tre -maxdepth 1 -name .pg_tre-patched -delete 2>/dev/null
find vendor/lime -maxdepth 1 -name lime -type f -delete 2>/dev/null
make -s PG_CONFIG=$PGB/pg_config clean >/dev/null 2>&1
make -s PG_CONFIG=$PGB/pg_config -j"$JOBS" \
     PG_CFLAGS="$SAN_CFLAGS" \
     SHLIB_LINK="vendor/tre/lib/.libs/libtre.a -lm $SAN_LDFLAGS" >"$ASAN_WORK.build.log" 2>&1 \
     || { tail -30 "$ASAN_WORK.build.log"; die "pg_tre ASan build failed"; }
grep -q tre_compile_progress_check vendor/tre/lib/tre-compile.c \
    || die "TRE progress-hook patch not applied (patches/tre-progress-hook.patch)"
grep -q tre_set_mbdecoder vendor/tre/lib/regcomp.c \
    || die "TRE mbdecoder patch not applied (patches/tre-mbdecoder.patch)"
grep -q 'tre_ctype("alpha")' vendor/tre/lib/tre-parse.c \
    || die "TRE icase-class patch not applied (patches/tre-icase-class.patch)"
grep -q 'item.pos_add_next' vendor/tre/lib/tre-match-backtrack.c \
    && grep -q 'num_slots' vendor/tre/lib/tre-match-approx.c \
    || die "TRE upstream-fixes patch not applied (patches/tre-upstream-fixes.patch)"
make -s PG_CONFIG=$PGB/pg_config install >/dev/null 2>&1 || die "pg_tre install failed"
# Capture first: under pipefail, `nm | grep -q` fails with SIGPIPE on a match.
syms=$(nm -D "$($PGB/pg_config --pkglibdir)/pg_tre.so")
[[ $syms == *" U __asan_init"* ]] || die "installed pg_tre.so is not ASan-instrumented"

# ---------------------------------------------------------------------------
# 3. Cluster.  Reports go to files so a crashed backend still leaves one.
#    detect_leaks=0: PostgreSQL leaks into long-lived contexts by design.
# ---------------------------------------------------------------------------
[ -d "$ASAN_WORK/data" ] && find "$ASAN_WORK/data" -delete
find "$ASAN_WORK/reports" -type f -delete
initdb -D "$ASAN_WORK/data" -E UTF8 --no-locale -U postgres --auth=trust >"$ASAN_WORK/initdb.log" 2>&1 \
    || { tail -20 "$ASAN_WORK/initdb.log"; die "initdb under ASan failed"; }
cat >>"$ASAN_WORK/data/postgresql.conf" <<EOF
port = $PORT
listen_addresses = ''
unix_socket_directories = '$ASAN_WORK'
max_connections = 30
autovacuum = off
EOF
pg_ctl -D "$ASAN_WORK/data" -l "$ASAN_WORK/server.log" -w start >/dev/null || die "server start failed"
trap 'pg_ctl -D "$ASAN_WORK/data" -m immediate stop >/dev/null 2>&1' EXIT

# ---------------------------------------------------------------------------
# 4. Regression suite.
# ---------------------------------------------------------------------------
export PGHOST=$ASAN_WORK PGPORT=$PORT PGUSER=postgres
log "running regression suite under ASan"
PG_CONFIG=$PGB/pg_config bash scripts/run-regress.sh "$@" 2>&1 | tee "$ASAN_WORK/regress.log"
rrc=${PIPESTATUS[0]}
sleep 2   # let a dying backend finish writing its report

# ---------------------------------------------------------------------------
# 5. Verdict.  A sanitizer report outranks a plain test failure.
# ---------------------------------------------------------------------------
reports=$(grep -l "ERROR: AddressSanitizer" "$ASAN_WORK"/reports/asan.* 2>/dev/null)
if [ -n "$reports" ]; then
    log "SANITIZER REPORTS: $(echo "$reports" | wc -l)"
    for f in $reports; do
        echo "---- $f"
        grep -m1 "ERROR: AddressSanitizer" "$f"
        grep -m1 -A12 -E "^(READ|WRITE) of size" "$f" | grep -E "^(READ|WRITE)|#[0-9]+ "
        grep -m1 -E "overflows this variable|is located" "$f"
    done
    exit 2
fi
[ "$rrc" -eq 0 ] || { log "regression failures (no sanitizer report)"; exit 1; }
log "clean: no sanitizer reports, all tests passed"
