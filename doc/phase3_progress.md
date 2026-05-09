# Phase 3 Progress Summary

## Completed Items

### 1. Build Infrastructure ✓
- **Makefile**: Lime generator built from `vendor/lime/lime.c`
- **Grammar generation**: `tre_grammar.{c,h}` generated from `tre_grammar.y`
- **Dependencies**: Query path objects properly depend on generated grammar
- **Clean rules**: `clean-grammar` target removes generated files

### 2. Regex Parser ✓
- **Tokenizer** (`src/query/tokens.c`): Hand-written mode-sensitive tokenizer
  - Handles escapes: `\n \t \\ \. \[ \] \( \) \{ \} \| \* \+ \? \^ \$`
  - Modes: default, inside `[...]`, inside `{m,n}`, inside `{~N}`
  - Phase 3: ASCII-only (multi-byte UTF-8 in Phase 3.5)
  
- **Grammar** (`src/query/tre_grammar.y`): Complete LALR(1) grammar
  - POSIX regex operators: `| * + ? {m,n} () [] . ^ $`
  - TRE approximate: `{~N}` for local edit budget
  - Character classes: `[abc]`, `[a-z]`, `[^0-9]`
  
- **AST Constructors** (`src/query/regex_ast.c`):
  - All `regex_ast_*` functions implemented
  - Normalized character class ranges
  - Debug pretty-printer `regex_ast_debug_dump()`
  
- **Parser Driver** (`src/query/parser.c`):
  - `tre_parse_regex()` wraps Lime parser + tokenizer
  - Error handling with `ctx->errmsg`
  - Memory managed via `ctx->mcxt`

### 3. tre_pattern Type ✓
- **Type Definition** (`src/util/type_pattern.c`):
  - Varlena layout: `[header][max_cost][cost_ins/del/subst][flags][pattern_len][pattern_bytes]`
  - Text I/O: `"pattern"`, `"pattern@k"`, `"pattern@k:ins,del,subst"`
  - Binary I/O: `recv`/`send` functions
  
- **Constructors**:
  - `tre_pattern(text)` → k=0
  - `tre_pattern(text, int4)` → k=N
  - `tre_pattern(text, int4, int4, int4, int4)` → full cost spec

### 4. %~~ Operator ✓
- **Operator**: `text %~~ tre_pattern`
- **Implementation** (`tre_match_scalar`): Delegates to legacy `tre_amatch()`
- **Opclass**: Wired into `tre_text_ops` as strategy 1
- **SQL**: Fully declared in `pg_tre--1.0.0.sql`

### 5. Debug UDFs ✓
- **`tre_parse_debug(text)`**: Returns AST dump for testing
- **`tre_extract_debug(text)`**: Returns trigram query tree (stub shows ALWAYS_TRUE)

### 6. Build Status ✓
- **All query/*.o compile cleanly**
- **Module links successfully**: `pg_tre.so` built
- **Zero warnings** in parser/AST/type code

## Incomplete Items

### 1. Trigram Extraction (Priority 1)
- **File**: `src/query/extract.c` is a stub
- **Algorithm**: Russ Cox's "Regular Expression Matching with a Trigram Index" (2012)
- **Properties per node**:
  - `emptyable`: can match empty string
  - `exact_set`: trigrams guaranteed to appear
  - `prefix_set`/`suffix_set`: 2-grams at boundaries
  - `match_set`: trigrams extracted (may be approximate)
- **Propagation rules**:
  - **LITERAL**: single trigram if ≥3 chars
  - **CONCAT**: cross-product prefix/suffix, union exact sets
  - **ALT**: union all sets
  - **REP**: mark emptyable, propagate child's match_set
  - **ANY/CLASS**: mark as "defeated" (too general)
- **Output**: AND-of-ORs `TrigramQuery` structure
- **Current**: Returns `always_true = true` (defeats extraction)

### 2. amgetbitmap Implementation (Priority 2)
- **File**: `src/am/amscan.c` (currently stubbed)
- **Logic**:
  1. Validate `ScanKey` (strategy 1, `tre_pattern` argument)
  2. Parse regex via `tre_parse_regex()`
  3. Extract `TrigramQuery` via `regex_extract_query(ctx, 0, &q)`
  4. For each AND conjunct:
     - For each OR disjunct: lookup posting via `pg_tre_upper_lookup()`
     - Materialize posting via `pg_tre_posting_materialize()`
     - Union sparisemaps within OR group
  5. Intersect all AND conjuncts
  6. Iterate final sparsemap, emit TIDs via `tbm_add_tuples()`
- **Current**: Stub raises "not yet implemented"

### 3. Posting Tree Stubs (Agent A Coordination)
- **File**: `src/pages/posting.c` has scaffolding stubs
- **Functions**:
  - `pg_tre_posting_scan_begin/next/end`: Return empty/error
  - `pg_tre_posting_materialize`: Returns empty sparsemap
- **Status**: Waiting for Agent A's Phase 1/2 ambuild implementation
- **Contract**: `include/pg_tre/posting.h` is stable

### 4. Differential Tests (Priority 3)
- **Test**: `test/sql/scan_exact.sql` not yet created
- **Fixture**: 100-row table, 20+ regex patterns
- **Views**:
  - `truth`: seq-scan via `tre_amatch(body, regex, 0)`
  - `via_index`: `body %~~ tre_pattern(regex, 0)` with `enable_seqscan=off`
- **Assert**: `array_agg(id)` equal for every pattern

## File Summary

### Created/Modified Files
- `Makefile`: Lime build, grammar generation, clean rules
- `src/query/tre_grammar.y`: Complete LALR(1) grammar
- `src/query/tokens.c`: Mode-sensitive tokenizer (ASCII Phase 3)
- `src/query/regex_ast.c`: AST constructors + debug dump
- `src/query/parser.c`: Parser driver wrapping Lime
- `src/query/extract.c`: Stub (ALWAYS_TRUE)
- `src/query/debug.c`: Debug UDFs
- `src/util/type_pattern.c`: tre_pattern type I/O + constructors
- `src/pages/posting.c`: Scaffolding stubs for Agent A's domain
- `include/pg_tre/regex_ast.h`: Added tokenizer + debug_dump prototypes
- `include/pg_tre/pg_tre.h`: Added fmgr.h, declared legacy UDFs
- `sql/pg_tre--1.0.0.sql`: tre_pattern type, %~~ operator, debug UDFs
- `test/sql/parser.sql`: Parser + type tests
- `STATUS.md`: Updated Phase 3 checklist

### Test Coverage
- **Parser tests**: 18 regex patterns in `test/sql/parser.sql`
- **Type tests**: I/O roundtrip, constructors
- **Operator test**: Basic %~~ via legacy tre_amatch
- **Extraction test**: Stub returns ALWAYS_TRUE

## Commits
1. `502cadc`: Build infrastructure, regex parser, and AST
2. `d9fb519`: tre_pattern type, %~~ operator, stub extract
3. `4804d81`: Debug UDFs for parser testing
4. `a9ce946`: Module links successfully
5. `1637c66`: Add parser test and update STATUS

## Next Steps (Priority Order)

1. **Implement trigram extraction** (`src/query/extract.c`)
   - Bottom-up property propagation
   - AND-of-ORs query tree generation
   - Differential test against known-good extraction

2. **Implement amgetbitmap scan** (`src/am/amscan.c`)
   - Parse + extract pipeline
   - Posting lookup + materialize
   - Sparsemap AND/OR operations
   - TID iteration

3. **Coordinate with Agent A**
   - Verify posting.h contract is sufficient
   - Test with real posting trees once ambuild lands

4. **Differential testing**
   - 100+ row fixture
   - 20+ regex patterns (literals, alternation, repetition, classes)
   - Assert index scan == seq scan for k=0

5. **EXPLAIN verification**
   - Test that planner picks "Bitmap Index Scan on ... using tre_index"
   - Verify `enable_seqscan=off` forces index usage

## Engineering Notes

- **Zero warnings**: All compiled code passes `-Wall -Werror=vla`
- **Memory safety**: All AST allocations tracked in `ctx->mcxt`
- **Grammar correctness**: Lime accepts the grammar without conflicts
- **Type safety**: RegexClass properly typed in Lime grammar
- **Phase 3 scope**: ASCII-only; UTF-8 in Phase 3.5
- **Agent coordination**: posting.h is the stable contract boundary

## Known Limitations (By Design)

- **Extraction defeated for**:
  - Patterns with `.*` (too general)
  - Patterns shorter than 3 bytes (no trigrams)
  - Character classes without literals (e.g., `[a-z]+`)
- **Phase 3 restrictions**:
  - `max_cost > 0` raises ERROR "approximate matching lands in Phase 5"
  - No UTF-8 (single-byte ASCII codepoints)
  - No `\d \w \s` escapes (raise syntax error)

## Build Verification

```bash
cd /home/gburd/ws/pg_tre
PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config make clean
PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config make
# pg_tre.so builds successfully, zero warnings in Phase 3 code
```

## Test Execution (Once Installed)

```bash
export PATH=/usr/bin:$PATH:~/.pgrx/18.3/pgrx-install/bin
export PGHOST=/tmp
psql -d postgres -f test/sql/parser.sql
```

Expected: All parser tests pass, extraction returns ALWAYS_TRUE, %~~ delegates to tre_amatch.
