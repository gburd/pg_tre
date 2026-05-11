# Phase 5 READ Implementation Summary

## Status: PARTIAL - Build Complete, Tests Not Run

### Implemented Components

1. **TrigramQuery Mode Extension** (regex_ast.h)
   - Added `TrigramQueryMode` enum (CNF/DNF)
   - Added `mode` field to `TrigramQuery` structure
   - Positional constraints (min_offset, max_offset) already present

2. **Universal Levenshtein Expansion** (src/query/uleven.c)
   - Implements k=0, k=1, k=2 edit distance expansion
   - Substitutions, insertions, deletions with deduplication
   - Fanout-limited to prevent DOS

3. **Navarro Tiling** (src/query/tiling.c)
   - Extracts trigram spine from AST literals
   - Partitions into k+1 tiles for pigeonhole principle
   - Outputs DNF TrigramQuery (OR across tiles)

4. **Approximate Extraction** (src/query/extract.c)
   - Extended `regex_extract_query` to handle k > 0
   - Calls `pg_tre_tile_query` for global-budget tiling
   - Initializes `mode` field in `accum_to_query`
   - TODO: uleven expansion integration (requires trigram bytes)

5. **Three-Tier Scan Logic** (src/am/amscan.c)
   - Removed k > 0 error check in `amrescan`
   - Added CNF vs DNF dispatch in `amgetbitmap`
   - Implemented `apply_tuple_bloom_filter` for tier-3 filtering
   - DNF mode: OR across tiles (AND within each tile)
   - Tier-1 range bloom: stub integration ready

6. **API Contracts** (include/pg_tre/)
   - posting.h: Added `pg_tre_posting_lookup_tuple_bloom`, `pg_tre_posting_lookup_positions`
   - range.h: Added `pg_tre_range_lookup`, `pg_tre_range_scan`, `pg_tre_range_bulkload`
   - tiling.h: Added `pg_tre_tile_query`
   - uleven.h: Added `pg_tre_uleven_expand`

7. **Stub Implementations**
   - range.c: Stubs for tier-1 APIs (return pass-through)
   - posting.c: Stub for `pg_tre_posting_lookup_positions`
   - Phase 5 WRITE has implemented `pg_tre_posting_lookup_tuple_bloom`

8. **Tests** (test/sql/p5_read.sql)
   - 50-row fixture with k=1 and k=2 variants
   - Differential testing: index-scan vs seq-scan
   - 21 test cases for k=1 and k=2 patterns
   - EXPLAIN verification for k>0 queries

### Critical Issue: tiling.o and uleven.o Not Linked

**Problem**: The new object files are listed in Makefile OBJS but are not being compiled into the shared library.

**Root Cause**: Unknown - possibly Makefile dependency tracking or PGXS quirk.

**Symptom**: `undefined symbol: pg_tre_tile_query` when loading pg_tre.so

**Workaround Attempted**: Clean + rebuild did not resolve.

**Next Steps**:
1. Check if PGXS caches the OBJS list
2. Try `make distclean` or manual .so removal
3. Verify tiling.o and uleven.o are created in src/query/
4. Add explicit dependency rules if needed

### Integration Notes for Phase 5 WRITE

**range.c Conflict**: The current range.c is a minimal stub. Phase 5 WRITE has a full implementation with different signatures. At merge:
- Use Phase 5 WRITE's range.c
- Update range.h to match their API if needed
- Verify `UpperTrigramIterator` typedef is in range.h

**posting.c**: Phase 5 WRITE has implemented `pg_tre_posting_lookup_tuple_bloom`. The stub for `pg_tre_posting_lookup_positions` should be replaced with their implementation.

**Positional Filtering**: Currently stubbed out (returns 0 positions). Phase 5 WRITE's position storage will enable this filter.

### Testing Plan (Once Build Fixed)

1. Install extension: `make install`
2. Restart PostgreSQL
3. Run `scripts/run-regress.sh` to execute all tests including p5_read.sql
4. Verify differential tests pass: index-scan rows == seq-scan rows
5. Check EXPLAIN output shows Bitmap Index Scan for k>0

### Remaining Work

1. **Fix Build**: Get tiling.o and uleven.o linked
2. **Uleven Integration**: Store trigram bytes alongside hashes in tiling, expand each tile's trigrams with uleven for k=1,2
3. **Tier-1 Integration**: Wire `pg_tre_range_lookup` into candidate block mask (currently stubbed)
4. **Positional Filtering**: Integrate `pg_tre_posting_lookup_positions` once Phase 5 WRITE implements it
5. **Merge Coordination**: Resolve range.c and posting.c with Phase 5 WRITE implementations

### Files Changed

**Created**:
- include/pg_tre/range.h
- include/pg_tre/tiling.h
- include/pg_tre/uleven.h
- src/query/tiling.c
- src/query/uleven.c
- src/pages/range.c (stub)
- test/sql/p5_read.sql

**Modified**:
- include/pg_tre/regex_ast.h (added TrigramQueryMode, mode field)
- include/pg_tre/posting.h (added tier-3 API declarations)
- src/query/extract.c (k>0 handling via tiling)
- src/am/amscan.c (CNF/DNF dispatch, tier-3 filtering)
- src/pages/posting.c (added positions stub)
- Makefile (added tiling.o, uleven.o, range.o to OBJS; added p5_read to REGRESS)

### Commit Strategy

Once build is fixed:
1. Commit: "Phase 5 READ: Add TrigramQuery mode and tiling API"
2. Commit: "Phase 5 READ: Implement uleven expansion"
3. Commit: "Phase 5 READ: Extend extract.c for k>0 tiling"
4. Commit: "Phase 5 READ: Add tier-3 bloom filtering in amscan"
5. Commit: "Phase 5 READ: Add stub APIs for range and positions"
6. Commit: "Phase 5 READ: Add p5_read test suite"

Each commit should build cleanly and pass existing tests.

### Known Limitations (Phase 5 Initial Cut)

- Uleven expansion not yet integrated (TODO Phase 5.1)
- Tier-1 range filtering is stubbed (pass-through)
- Positional filtering is stubbed (returns 0 positions)
- Local {~m} budget handling is simplified (global budget only)
- sparsemap_offset fast-path not implemented (documented TODO for Phase 8)
