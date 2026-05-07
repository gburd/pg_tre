# pg_tre

A PostgreSQL native index access method for approximate-regex
matching, backed by the [TRE](https://github.com/laurikari/tre)
library and three tiers of q-gram / bloom-filter / sparse-bitmap
filtration.

Status: under active development; see [STATUS.md](STATUS.md) for the
live phase state.

## Goal

    CREATE EXTENSION pg_tre;
    CREATE INDEX docs_tre ON docs USING tre (body);
    SELECT * FROM docs
     WHERE body %~~ tre_pattern('enviro.{~2}ment', 2);

The `tre` access method maintains a per-trigram sparsemap inverted
index with per-tuple bloom signatures and BRIN-style range
summaries, enabling sub-linear approximate regex search on text
columns.

## Requirements

- PostgreSQL 18 or newer.
- GNU make, a working C compiler, autoconf/automake/libtool/gettext/m4
  for the vendored TRE build.
- Git submodules: `vendor/tre` (TRE 0.9.0) and `vendor/lime` (Lime
  parser generator).

## Build

    git clone --recurse-submodules https://codeberg.org/gregburd/pg_tre.git
    cd pg_tre
    make PG_CONFIG=/path/to/pg_config
    sudo make PG_CONFIG=/path/to/pg_config install

## Run

Add the extension to `shared_preload_libraries` so its custom WAL
resource manager can register (required for the index AM; the
legacy UDFs work without preload):

    # postgresql.conf
    shared_preload_libraries = 'pg_tre'

Restart, then:

    CREATE EXTENSION pg_tre;

## What's available today

See [STATUS.md](STATUS.md).  The short version:

- Legacy UDFs `tre_amatch`, `tre_amatch_cost`, `tre_amatch_detail`,
  `tre_version` (inherited from 0.1.0) -- fully functional.
- `CREATE ACCESS METHOD tre` is registered.
- `CREATE INDEX ... USING tre` succeeds and creates an empty
  (stub) index.
- INSERT and SELECT through the index raise
  `ERROR: pg_tre ... not yet implemented` until later phases land.

## Documentation

- [doc/design.md](doc/design.md) -- architecture overview.
- [doc/onpage_format.md](doc/onpage_format.md) -- on-disk layout
  specification.

## License

pg_tre is MIT (see [LICENSE](LICENSE)).  Third-party components are
MIT (sparsemap), BSD-like (TRE), and public-domain (Lime).  See
[NOTICE](NOTICE) for details.
