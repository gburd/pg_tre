# pg_tre Regex Parser Fuzzing

libFuzzer harness for the regex parser surface:

- `tre_parse_regex()` — Lime LALR(1) parser
- `regex_extract_query()` — trigram extraction for k ∈ {0,1,2}
- `pg_tre_tile_query()` — Navarro tiling for approximate matching

## Requirements

- clang with libFuzzer support (clang 12+)
- AddressSanitizer (built into clang)
- pg_tre source tree built once with `make PG_CONFIG=...` so the Lime
  grammar is generated at `src/query/tre_grammar.{c,h}`.
- A PostgreSQL 18 install reachable via `pg_config` (default:
  `~/.pgrx/18.3/pgrx-install/bin/pg_config`).  The harness needs the
  PG18 server headers and the static archives `libpgcommon.a` and
  `libpgport.a` from `$(pg_config --libdir)`.

## Build

```bash
cd fuzz
make -f Makefile.fuzz
```

Produces `./pg_tre_fuzz`.

Override `PG_CONFIG` to pick a non-default install:

```bash
make -f Makefile.fuzz PG_CONFIG=/path/to/pg_config
```

## Run

```bash
./pg_tre_fuzz corpus -max_total_time=900 -rss_limit_mb=2048 -detect_leaks=0
```

- `corpus/` contains seed inputs (named files) plus mutation
  discoveries from prior runs (hex-named files).
- `-max_total_time=N` caps the campaign at N seconds.
- `-detect_leaks=0` is required for long campaigns; see the
  "MemoryContext lifecycle" caveat below.
- Other libFuzzer stop conditions: crash or timeout.

## Layout

- `parse_regex_fuzz.c` — `LLVMFuzzerTestOneInput` entry point; uses real
  PostgreSQL headers (`postgres.h`, `lib/stringinfo.h`,
  `utils/memutils.h`, `pg_tre/regex_ast.h`, `pg_tre/tiling.h`).
- `pg_backend_stub.c` — thin shim for the small set of backend symbols
  that `libpgcommon.a` does not supply (see "What is and isn't real"
  below).
- `Makefile.fuzz` — out-of-tree build script; links against
  `libpgcommon.a` and `libpgport.a`.
- `corpus/` — seed corpus and mutation carryover.
- `create_corpus.sh` — regenerate the seed corpus.

## What is and isn't real

The harness no longer rolls its own `palloc` / `StringInfo`.  Those
come straight from the PostgreSQL static archives:

| symbol                            | source                |
|-----------------------------------|-----------------------|
| `palloc`, `palloc0`, `pfree`, `repalloc`, `pstrdup`, `pnstrdup` | `libpgcommon.a` (frontend `fe_memutils.c`) |
| `initStringInfo`, `appendStringInfo*`, `psprintf`              | `libpgcommon.a`       |
| `pg_utf_mblen_private`            | `libpgcommon.a`       |
| `pg_snprintf`, `pg_fprintf`, `pg_strerror` | `libpgport.a`         |

What `pg_backend_stub.c` still provides, because libpgcommon does not:

- `CurrentMemoryContext`, `PG_exception_stack`, `error_context_stack`
- `AllocSetContextCreateInternal`, `MemoryContextDelete`,
  `MemoryContextReset` (no-op contexts; see below)
- `errstart`/`errstart_cold`/`errfinish`/`errcode`/`errmsg`/
  `errmsg_internal`/`errdetail`/`errhint`/`pg_re_throw` (longjmp out
  via `pg_fuzz_error_jmp`)
- `ExceptionalCondition` (assertion handler)
- `pg_tre_max_extraction_fanout` (the GUC backing variable normally in
  `src/module.c`)

## Limitations

### MemoryContext lifecycle is fake

`libpgcommon`'s `palloc` is malloc-backed and has no per-context
tracking.  `MemoryContextDelete` cannot free the allocations belonging
to a context the way the real backend's AllocSet would, so each fuzz
iteration leaks all of its `palloc()` chunks.  Run with
`-detect_leaks=0` (or `ASAN_OPTIONS=detect_leaks=0`) for long
campaigns.

ASan still red-zones every individual `palloc` chunk, so heap UAF and
out-of-bounds bugs on individual allocations are detected.  What this
harness *cannot* detect is cross-context UAF -- a use-after-free that
only manifests when the chunk's owning MemoryContext is freed.  For
that you would need to link the harness as a backend extension and run
it inside a real backend (`shared_preload_libraries`), where AllocSet
provides real per-context cleanup and `MemoryChunk` headers.

### Frontend / backend split

Several backend-only headers (`utils/elog.h`, `utils/memutils.h`,
`nodes/memnodes.h`) are included with `FRONTEND` undefined.  pg_tre's
sources expect backend semantics (e.g. inline `MemoryContextSwitchTo`
that reads a real `CurrentMemoryContext` global), and we provide
exactly that global plus the lifecycle stubs.  The harness compiles
without defining `FRONTEND`.

## Continuous fuzzing

Not yet wired into CI.  When ready, add a job that runs a short
campaign (5 minutes, `-max_total_time=300 -detect_leaks=0`) on every
push, with the corpus persisted as a CI artifact.
