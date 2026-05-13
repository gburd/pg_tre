# pg_tre Regex Parser Fuzzing

libFuzzer harness for the regex parser surface:

- `tre_parse_regex()` — Lime LALR(1) parser
- `regex_extract_query()` — trigram extraction for k ∈ {0,1,2}
- `pg_tre_tile_query()` — Navarro tiling for approximate matching

## Requirements

- clang with libFuzzer support (clang 12+)
- AddressSanitizer (built into clang)
- pg_tre source tree built once: `make PG_CONFIG=...`

## Build

```bash
cd fuzz
make -f Makefile.fuzz
```

Produces `./pg_tre_fuzz`.

## Run

```bash
./pg_tre_fuzz corpus -max_total_time=900 -rss_limit_mb=2048
```

- `corpus/` contains seed inputs (named files) plus mutation
  discoveries from prior runs (hex-named files).
- `-max_total_time=N` caps the campaign at N seconds.
- libFuzzer's stop conditions: crash, leak (with
  `-detect_leaks=1`), or timeout.

## Layout

- `parse_regex_fuzz.c` — `LLVMFuzzerTestOneInput` entry point.
- `memutils_stub.c` — minimal Postgres backend shim:
  `palloc`/`pfree`, MemoryContext API, error handling via
  `setjmp`/`longjmp`, `StringInfo`, `pg_snprintf`, etc.
- `Makefile.fuzz` — out-of-tree build script.
- `corpus/` — seed corpus and mutation carryover.
- `create_corpus.sh` — regenerate the seed corpus.

## Limitations

The harness uses local stub structs for `TreParseCtx` and
`TrigramQuery` rather than including pg_tre's headers.  This
means the harness can drift from the in-tree types if those
structs evolve; the fix is to switch to real headers and
compile against the pg_tre source tree directly.  Tracked
in `STATUS.md` under the v1.1 followups.

## Continuous fuzzing

Not yet wired into CI.  When ready, add a job that runs a
short campaign (5 minutes, `-max_total_time=300`) on every
push, with the corpus persisted as a CI artifact.
