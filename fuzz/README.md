# Fuzzing Infrastructure for pg_tre Regex Parser

## Overview

This directory contains libFuzzer-based fuzzing for the pg_tre regex parser surface:
- `tre_parse_regex()` - Lime LALR(1) parser
- `regex_extract_query()` - trigram extraction for k ∈ {0,1,2}
- `pg_tre_tile_query()` - Navarro tiling for approximate matching

## Status

**Current state:** Initial fuzzing infrastructure created but not yet complete due to memory leak issues in error handling paths.

### Issues Discovered

1. **Parser Memory Leak**: The Lime-generated parser (`pg_tre_rx_parseAlloc`) is allocated with `malloc()` but not freed when `ereport(ERROR)` triggers via `longjmp` during parsing. This causes approximately 2.4KB leak per error case.

2. **Invalid UTF-8 Handling**: The UTF-8 decoder in `src/util/utf8.c` calls `ereport(ERROR)` on invalid sequences, which is correct for production but requires careful cleanup handling in fuzzing.

3. **Execution Rate**: When running with `-detect_leaks=0`, the fuzzer achieves ~2,000-2,500 executions/second, discovering 200+ new corpus entries in 30 seconds before hitting OOM at 2GB RSS.

### What Works

- libFuzzer integration compiles cleanly with clang + ASan + UBSan
- `setjmp/longjmp` error handling wrapper catches `ereport(ERROR)` without crashing
- Seed corpus of 26 regex patterns covering basic operators, character classes, anchors, and approximate matching
- Coverage tracking shows 67 edges across 352 features

## Building

**Note:** This requires clang with libFuzzer support (clang 12+).

```bash
# Compile pg_tre first
make PG_CONFIG=<path>

# Build fuzzer (when complete)
cd fuzz
make -f Makefile.fuzz

# Run fuzzer (when complete)
./pg_tre_fuzz -detect_leaks=0 -rss_limit_mb=4096 corpus/ -max_total_time=1800
```

## Required Fixes

Before this fuzzer can run for 30+ minutes without OOM:

1. **Fix parser cleanup on error**: Modify `src/query/parser.c` to ensure `pg_tre_rx_parseFree()` is called even when `ereport(ERROR)` is triggered. Options:
   - Add PG_TRY/PG_CATCH block around parsing
   - Track parser globally and free in error handler
   - Change parser to use `palloc()` instead of `malloc()` so MemoryContext cleanup handles it

2. **Add parser leak test**: Create a regression test that calls `tre_parse_regex()` in a loop with invalid inputs and verifies no memory growth.

3. **Increase corpus diversity**: Add UTF-8 multibyte patterns, deeply nested groups, large repetition counts, and patterns from real-world SQL queries.

## Expected Results (once fixes are complete)

Target metrics for a production-ready fuzzing run:
- **Duration**: 30+ minutes (1800+ seconds)
- **Executions**: ≥1 million test cases
- **Coverage**: 80+ edges (baseline: 67)
- **Crashes**: Zero after fixes
- **Peak RSS**: < 1GB with proper cleanup

## Corpus

The seed corpus (`corpus/`) contains 26 hand-crafted patterns:
- Basic literals and operators (`*`, `+`, `?`, `|`)
- Repetition (`{m}`, `{m,n}`)
- Character classes (`[abc]`, `[a-z]`, `[^...]`)
- Anchors (`^`, `$`)
- Approximate matching (`{~k}` for k=1,2)
- Edge cases (empty, unclosed brackets, dangling operators)

LibFuzzer will mutate these to discover new coverage.

## Integration with CI

Once stable, add to `.github/workflows/ci.yml`:
```yaml
- name: Fuzz regex parser
  run: |
    cd fuzz
    make -f Makefile.fuzz
    ./pg_tre_fuzz -max_total_time=300 -detect_leaks=1 corpus/
```

## References

- libFuzzer tutorial: https://llvm.org/docs/LibFuzzer.html
- Navarro tiling paper: "Fast and Flexible String Matching by Combining Bit-parallelism and Suffix Automata" (Navarro & Raffinot, 2000)
- PostgreSQL error handling: https://www.postgresql.org/docs/current/error-handling.html
