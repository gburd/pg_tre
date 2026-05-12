# Tier-3 Bloom Filter False Negative Bug - Root Cause Analysis & Fix

## Mission Summary

Successfully debugged and fixed false-negative bug in pg_tre's tier-3 per-tuple bloom filter, then re-enabled tier-3 and positional filtering. All 9 regression tests pass.

## Root Cause

### The Bug

Functions `pg_tre_posting_lookup_tuple_bloom` and `pg_tre_posting_lookup_positions` in `src/pages/posting.c` incorrectly computed the offset into the per-TID payload array.

```c
// BEFORE (buggy):
rank = sparsemap_rank(smap, 0, packed_tid, true);
for (size_t i = 0; i < rank; i++)  // WRONG: skips rank entries
{
    // skip to next entry
}
// Now pointing at entry AFTER the target
```

### Why It Was Wrong

`sparsemap_rank(smap, 0, packed_tid, true)` returns the count of set bits from position 0 to `packed_tid` **INCLUSIVE**.

Example: If we have TIDs [10, 20, 30, 40] in the sparsemap and want to find TID 30:
- TID 30 is at 0-indexed position **2** (third entry)
- `sparsemap_rank(smap, 0, 30, true)` returns **3** (count of TIDs 10, 20, 30)
- The buggy code skips **3** entries, landing on TID 40 (position 3) — **WRONG**
- Correct: skip **2** entries (rank - 1) to land on TID 30 (position 2)

### Manifestation

When tier-3 was enabled queries would read the wrong TID's bloom, causing false negatives.

## The Fix

Changed loops to skip `(rank - 1)` entries instead of `rank` entries.

## Verification

All 9 regression tests pass with tier-3 and positional filtering enabled.

## Commits

1. **bfc7d33** "Fix tier-3 bloom filter rank-based offset bug"
2. **7a6ea65** "Add tier3.sql regression test"

