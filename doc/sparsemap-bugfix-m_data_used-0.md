# Sparsemap Bug: Crash when m_data_used=0 with garbage chunk_count

## Summary

Fixed crash in `sparsemap_intersection`, `sparsemap_union`, `sparsemap_maximum`, and `sparsemap_rank` when passed a sparsemap with `m_data_used = 0` but non-zero (garbage) chunk count.

## Root Cause

`__sm_get_chunk_count(map)` reads from `map->m_data[0]` (first 4 bytes) regardless of `map->m_data_used`. When a sparsemap has `m_data_used = 0` (improperly initialized or corrupted state), the chunk count reads uninitialized memory, causing functions that iterate chunks to read past the buffer end.

## Reproduction

1. Enable tier-3 bloom filter in `src/am/amscan.c` (change `if (false && ...)` to `if (...)`)
2. Build index with diverse content
3. Run bitmap index scan triggering multiple `sparsemap_intersection` calls

Crash observed at:
```
#0  sparsemap_intersection (a=..., b=...) at src/util/sparsemap.c:2508
    BUG: chunk a[16] pointer past end of map (capacity=260, used=0)
#1  pg_tre_amgetbitmap (scan=..., tbm=...) at src/am/amscan.c:558
```

## Fix

Added defensive checks in:
- `sparsemap_intersection`: return NULL if either input has `m_data_used = 0`
- `sparsemap_union`: treat maps with `m_data_used = 0` as having 0 chunks  
- `sparsemap_maximum`: return 0 if `m_data_used = 0`
- `__sm_rank_vec`: return appropriate value if `m_data_used = 0`

A valid empty sparsemap has `m_data_used = SM_SIZEOF_OVERHEAD` (4 bytes) with `chunk_count = 0` stored in those bytes. If `m_data_used = 0`, the map was not properly initialized via `sparsemap_clear()`.

## Files Modified

- `src/util/sparsemap.c`: Added `m_data_used == 0` checks in intersection (line 2473), union (line 2994), maximum (line 1733), and rank (line 3506)
- `src/am/amscan.c`: Un-gated tier-3 bloom filter (line 675)

## Testing

Before fix:
```sql
CREATE TABLE sm_test(id serial, body text);
INSERT INTO sm_test(body) SELECT md5(i::text) FROM generate_series(1,100) i;
CREATE INDEX sm_test_idx ON sm_test USING tre (body);
SELECT * FROM sm_test WHERE body %~~ tre_pattern('find', 0);
-- Result: SIGSEGV in sparsemap_intersection
```

After fix:
- Same query runs without crash
- `make localcheck` passes (6 tests)
- Tier-3 bloom filter active and functional

## Upstream Consideration

This bug exists in the vendored `src/util/sparsemap.c` (from an external sparsemap library). The library does not properly handle the case where a sparsemap struct exists but its buffer is uninitialized (`m_data_used = 0`).

Recommended upstream fix: Either document that `__sm_get_chunk_count` is only valid when `m_data_used >= SM_SIZEOF_OVERHEAD`, or modify `__sm_get_chunk_count` to return 0 when `m_data_used == 0`.
