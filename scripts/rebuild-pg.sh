#!/usr/bin/env bash
# Rebuild PG 19devel (cf-5556) inside the nix devShell closure, then build+test pg_tre.
set -euo pipefail

PGSRC=/home/gburd/ws/postgres/cf-5556
PREFIX=/home/gburd/ws/postgres/cf-5556/_rebuild_install
BUILD=/home/gburd/ws/postgres/cf-5556/_rebuild_build
PGTRE=/home/gburd/ws/pg_tre

echo "=== which gcc/meson/ninja (should be nix-provided) ==="
which gcc meson ninja || true
gcc --version | head -1

cd "$PGSRC"

# Fresh meson setup into a clean build/prefix so we don't clobber the cached build dir.
if [ ! -f "$BUILD/build.ninja" ]; then
  echo "=== meson setup -> $PREFIX ==="
  meson setup "$BUILD" "$PGSRC" \
    --prefix="$PREFIX" \
    --libdir=lib64 \
    -Ddebug=true -Doptimization=0 -Dcassert=true \
    -Dicu=enabled -Dlz4=enabled -Dzstd=enabled -Dlibxml=enabled \
    -Dreadline=enabled -Dssl=openssl -Duuid=e2fs \
    -Dplperl=disabled -Dplpython=disabled -Dpltcl=disabled \
    -Dnls=disabled -Dllvm=disabled -Dtap_tests=disabled \
    2>&1 | tail -25
fi

echo "=== ninja build (this may take a while) ==="
ninja -C "$BUILD" 2>&1 | tail -8

echo "=== ninja install ==="
ninja -C "$BUILD" install 2>&1 | tail -5

echo "=== verify freshly built pg_config + binaries run with NO LD_LIBRARY_PATH ==="
"$PREFIX/bin/pg_config" --version
"$PREFIX/bin/postgres" --version
"$PREFIX/bin/initdb" --version
"$PREFIX/bin/psql" --version

echo "REBUILD_OK"
