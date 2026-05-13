# pg_tre developer notes

Build + layout reference for contributors. End users want
[`README.md`](README.md) and [`doc/pg_tre.md`](doc/pg_tre.md)
instead.

## Build

    PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config
    make PG_CONFIG=$PG_CONFIG
    sudo make PG_CONFIG=$PG_CONFIG install
    scripts/run-regress.sh           # local regression
    PG_CONFIG=$PG_CONFIG make check  # same via Makefile

Requires autoconf/automake/libtool/gettext/m4 for the vendored TRE
autotools build; PG18+ only.

## Submodules

    vendor/tre      https://github.com/laurikari/tre (pinned v0.9.0)
    vendor/lime     https://codeberg.org/gregburd/lime

Clone with `--recurse-submodules` or run `git submodule update --init`.

## Layout

- `include/pg_tre/`   public headers (page.h, xlog.h, amapi.h,
  pg_tre.h, sparsemap.h, chunk_codec.h, popcount.h, tre_match.h,
  pattern_cache.h).
- `src/am/`           IndexAmRoutine handler + callbacks.
- `src/pages/`        page readers/writers (meta, upper, posting,
  payload, range, pending).
- `src/wal/`          custom rmgr.
- `src/query/`        regex parser (Lime), tokenizer, extraction,
  tiling, universal-Levenshtein expansion.
- `src/util/`         sparsemap.c, tre_match.c, pattern_cache.c.
- `vendor/tre/`       submodule -- TRE library.
- `vendor/lime/`      submodule -- Lime LALR(1) generator.

## Running

Add `shared_preload_libraries = 'pg_tre'` to `postgresql.conf`
before `CREATE EXTENSION pg_tre;`.  Without preload, only legacy
UDFs (`tre_amatch*`, `tre_version`) are available; the AM's rmgr
isn't registered.

## Status / roadmap

See `doc/design.md` for the architecture and `CHANGELOG.md`
for what shipped in each release.
