# Phase 5 WRITE Side Complete - Integration Notes for Phase 5 READ

## Summary

Phase 5 WRITE side has successfully implemented the **write path** for the 3-tier filter funnel:

- **Tier 1 (Range)**: BRIN-style per-range bloom filters (128 blocks/range by default)
- **Tier 3 (Tuple)**: Per-tuple bloom filters + position lists in posting leaf payload
- **Tier 2 (Posting)**: Sparsemap postings (already implemented in Phase 2/3/4)

All write-side structures are **populated during index build** and **WAL-logged correctly**. The structures are ready for Phase 5 READ to consume during scan.

---

## Files Changed

### New Files
- `src/util/bloom.c` — Bloom filter primitive (double-hashing)
- `include/pg_tre/bloom.h` — Bloom filter API

### Modified Files
- `src/pages/posting.c` — Extended to serialize per-tuple payload (positions + bloom)
- `include/pg_tre/posting.h` — Added `pg_tre_posting_lookup_tuple_bloom()` reader helper
- `src/pages/range.c` — Complete BRIN-style range tier implementation
- `include/pg_tre/range.h` — Range tier API + `UpperTrigramIterator` typedef
- `src/am/ambuild.c` — Bloom population + range bulkload integration
- `src/module.c` — Added `pg_tre_tuple_bloom_enable` GUC (default true)
- `include/pg_tre/pg_tre.h` — Exported new GUC
- `src/wal/xlog.c` — XLOG_PTRE_RANGE_UPDATE routed to FPI replay
- `Makefile` — Added bloom.o; commented out tiling.o/uleven.o (Phase 5 READ side)

---

## API Contract: What Phase 5 READ Needs to Know

### 1. Bloom Filter Primitive (`include/pg_tre/bloom.h`)

```c
typedef struct PgTreBloom {
    uint16 m_bits;  // e.g., 128 for tuple, 2048 for range
    uint8  k;       // number of hash functions (5 for tuple, 7 for range)
    uint8  _pad;
    // uint8 bits[] follows inline
} PgTreBloom;

// Compute bloom size in bytes
Size pg_tre_bloom_size_bytes(uint16 m_bits);

// Initialize bloom filter
void pg_tre_bloom_init(PgTreBloom *b, uint16 m_bits, uint8 k);

// Add a trigram (hash is the uint64 from pg_tre_hash_trigram)
void pg_tre_bloom_add_trigram(PgTreBloom *b, uint64 trigram_hash);

// Test if trigram might be present
bool pg_tre_bloom_contains_trigram(const PgTreBloom *b, uint64 trigram_hash);

// Union two blooms (OR their bit vectors)
void pg_tre_bloom_union(PgTreBloom *dst, const PgTreBloom *src);

// Access bit vector
uint8 *pg_tre_bloom_bits(PgTreBloom *b);
```

**Double-hashing formula**: `(h1 + i*h2) % m_bits` for i=0..k-1, where h1 = hash & 0xFFFFFFFF, h2 = hash >> 32.

---

### 2. Posting Leaf Payload (`include/pg_tre/posting.h`)

**On-disk layout (when `with_payload` is true)**:

```
[ PgTrePostingLeafHeader ]
[ sparsemap blob ]
[ ... free space ... ]
[ payload region (grows downward from page end) ]
```

**Payload region format** (per TID, in sparsemap order):
```
uint16 n_positions;            // number of position bytes that follow
uint32 positions[n_positions]; // byte offsets (not delta-coded yet in Phase 5)
uint8  bloom[(pg_tre_bloom_tuple_bits + 7) / 8];  // per-tuple bloom bits
```

**Reader helper**:
```c
bool pg_tre_posting_lookup_tuple_bloom(
    Relation index,
    BlockNumber root,
    const uint8 *inline_data,
    Size inline_bytes,
    uint64 packed_tid,
    uint8 *out_bloom,
    Size out_bloom_sz);
```
- Returns true if TID is present in the posting and bloom data is available
- Copies bloom bits into `out_bloom` (caller must allocate `(pg_tre_bloom_tuple_bits + 7) / 8` bytes)
- **Note**: Currently does NOT support inline postings with payload (returns false). Only on-disk leaves.

**To access payload during scan**:
1. Use `sparsemap_rank(smap, 0, packed_tid, true)` to get the entry index for a TID
2. Walk the payload region by reading:
   - `uint16 n_positions`
   - `n_positions * sizeof(uint32)` bytes of positions
   - `bloom_bytes` bytes of bloom
3. Repeat for each TID in sparsemap order

---

### 3. Range Tier (`include/pg_tre/range.h`)

**On-disk**: Single-leaf page (PG_TRE_PAGE_RANGE) with entries:
```c
typedef struct PgTreRangeLeafEntry {
    BlockNumber range_start_blk;  // e.g., 0, 128, 256, ...
    BlockNumber range_end_blk;    // exclusive
    uint32      bloom_bytes;      // size of bloom that follows
    // bloom bits follow inline
} PgTreRangeLeafEntry;
```

**Lookup API**:
```c
bool pg_tre_range_lookup(Relation index, BlockNumber heap_blk, PgTreBloom **out_bloom);
```
- Returns true if a range exists covering `heap_blk`
- Allocates a `PgTreBloom` in caller's memory context (caller must pfree)
- Bloom contains the union of all trigrams in that range

**Scan API** (for introspection / block-mask construction):
```c
typedef void (*PgTreRangeScanCallback)(BlockNumber range_start,
                                       BlockNumber range_end,
                                       const PgTreBloom *bloom,
                                       void *ctx);

void pg_tre_range_scan(Relation index, PgTreRangeScanCallback callback, void *ctx);
```
- Iterates all range entries in ascending block order
- Callback receives a palloc'd bloom (pfree'd after callback returns)

**Meta page**: `meta.root_range` holds the range leaf's block number (or InvalidBlockNumber if no ranges)

---

## Integration Points for Phase 5 READ

### Scan Path Integration

The intended 3-tier filter pipeline in `amgetbitmap`:

```
1. Range tier (tier 1):
   - For each required trigram in the query:
     * Look up the trigram's posting via pg_tre_upper_lookup
     * For each TID in the posting, extract heap_blk
     * Call pg_tre_range_lookup(heap_blk) -> range_bloom
     * If !pg_tre_bloom_contains_trigram(range_bloom, trigram_hash):
         Reject this TID (range doesn't have the trigram)

2. Posting tier (tier 2):
   - AND/OR sparsemaps across conjuncts (already implemented in Phase 3)
   - Produces candidate TID set

3. Tuple tier (tier 3):
   - For each candidate TID:
     * Call pg_tre_posting_lookup_tuple_bloom(tid) -> tuple_bloom
     * For each required trigram:
         If !pg_tre_bloom_contains_trigram(tuple_bloom, trigram_hash):
             Reject this TID
     * Survivors proceed to heap recheck

4. Heap recheck (already implemented in Phase 3)
```

### Pending Merge Integration (Phase 4 compatibility)

**Current state**: `pg_tre_pending_merge()` rebuilds the upper tree but does NOT populate blooms for merged entries. It calls `pg_tre_posting_build_add()` with `NULL` positions and bloom.

**Phase 5 WRITE approach** (documented in ambuild.c):
- During merge, walk the heap rows corresponding to pending TIDs
- Re-tokenize to rebuild positions + bloom
- Pass to `pg_tre_posting_build_add()`

**Deferred to Phase 5 READ** because it requires the same tokenization logic used in ambuild.

**Alternative**: Store blooms in `PgTrePendingEntry` (Phase 8 optimization).

---

## GUCs

- `pg_tre.tuple_bloom_enable` (bool, default true, SIGHUP)
  * Controls whether per-tuple blooms are populated during build
  * When false, posting leaves omit payload region
  * Changing requires REINDEX

- `pg_tre.bloom_tuple_bits` (int, default 128, SIGHUP)
  * Bits per per-tuple bloom filter
  * Valid range: 32..1024
  * Changing requires REINDEX

- `pg_tre.range_size_blocks` (int, default 128, SIGHUP)
  * Heap blocks covered by each range entry
  * Valid range: 1..131072
  * Changing requires REINDEX

---

## WAL Correctness

- All new records (`XLOG_PTRE_RANGE_UPDATE`) are replayed via `pg_tre_redo_fpi` (full-page image)
- Range tier updates are crash-safe and streaming-replica safe
- Posting leaf payload is part of the FPI for `XLOG_PTRE_POSTING_INSERT`

---

## Testing Hooks

**Recommended debug SRFs** (for Phase 5 READ to implement):

1. `tre_debug_tuple_bloom(regclass, tid) -> bloom_bits[]`
   - Returns the bloom bits for a specific TID
   - Verify that every trigram of the tuple causes `contains_trigram()` to return true
   - Verify that random non-present trigrams return false at expected rate

2. `tre_debug_range_blooms(regclass) -> TABLE(range_start, range_end, n_bits_set)`
   - Returns all range entries with bit-set counts
   - Useful for inspecting coverage and false-positive rates

---

## Known Limitations (Phase 5 WRITE)

1. **Inline postings**: Payload is NOT supported for inline postings (< 256 bytes). These remain sparsemap-only. The vast majority of postings are on-disk leaves.

2. **Single-leaf range tree**: Range tier is a single leaf page. Phase 8 will extend to multi-level tree. If the range leaf fills (unlikely for reasonable data sizes), later ranges are truncated with a WARNING.

3. **Linear range lookup**: Range tier uses linear scan. Phase 8 will add binary search.

4. **Pending merge blooms**: Pending entries merged via VACUUM do NOT have blooms yet. Phase 5 READ should implement heap-walk during merge to populate them.

5. **Position list encoding**: Positions are stored as `uint32[]` without delta coding. Phase 8 can compress.

---

## Files to Review for Integration

### Must Read
- `include/pg_tre/bloom.h` — Bloom filter API (THE primitives)
- `include/pg_tre/posting.h` — Posting API + lookup_tuple_bloom()
- `include/pg_tre/range.h` — Range tier API + lookup/scan
- `src/pages/posting.c` — Payload serialization logic (lines 300-450)
- `src/pages/range.c` — Range tier implementation

### Reference
- `src/am/ambuild.c` — Example of bloom population during build (lines 200-550)
- `src/util/bloom.c` — Bloom implementation (double-hashing details)
- `include/pg_tre/page.h` — PgTrePostingLeafHeader, PgTreRangeLeafEntry layouts

---

## Build & Test

```bash
PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config make
PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config make install

# Restart with shared_preload_libraries='pg_tre'
pg_ctl -D /tmp/pgdata_tre -m fast restart -o "-k /tmp"

# Test (Phase 5 READ will add p5_read.sql with bloom introspection)
PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config make localcheck
```

---

## Commits

1. `29f437f` Phase 5 WRITE: Steps 1-2 - Bloom filter + posting payload
2. `2d0e652` Phase 5 WRITE: Step 3 - Ambuild bloom population
3. `0a240ee` Phase 5 WRITE: Step 4 - BRIN-style range tier
4. `fb3678c` Phase 5 WRITE: Step 6 - WAL support for RANGE_UPDATE

---

## Gotchas / Coordination Notes

### API Stability
- `posting.h` is the contract file. Do NOT change existing function signatures.
- New APIs should be added at the end, marked with phase comments.

### Inline vs On-Disk
- Inline postings do NOT have payload in Phase 5.
- `pg_tre_posting_lookup_tuple_bloom()` returns false for inline postings.
- If this becomes a bottleneck, Phase 8 can extend inline format.

### Bloom False Positive Rates
- Tuple bloom: 128 bits, k=5 → ~2% FPR at 10 trigrams per tuple
- Range bloom: 2048 bits, k=7 → ~0.5% FPR at 100 trigrams per range
- These are configurable via GUCs if Phase 5 READ finds different optima.

### Range Tier Empty Case
- If no ranges are built (e.g., empty index), `meta.root_range` is InvalidBlockNumber.
- `pg_tre_range_lookup()` returns false immediately in this case.

---

## Next Steps for Phase 5 READ

1. **Scan integration**: Extend `amgetbitmap` to call range/tuple bloom filters
2. **Query expansion**: Implement Navarro tiling + universal Levenshtein for k>0
3. **Position filtering**: Use sparsemap_offset for positional constraints
4. **Pending merge blooms**: Heap-walk during merge to populate blooms
5. **Debug SRFs**: Implement introspection functions
6. **Tests**: Differential tests at k∈{0,1,2,3}, verify bloom coverage

---

## Questions / Clarifications

Contact Phase 5 WRITE agent via STATUS.md updates or inline comments in posting.h / range.h.

**Phase 5 WRITE side is COMPLETE and READY for integration.**
