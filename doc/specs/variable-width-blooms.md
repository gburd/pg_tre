# Variable-width per-tuple blooms — v1.3 design spec

**Status:** draft, targeting pg_tre 1.3.

**Owner:** unassigned.

**Prerequisite:** ~~multi-leaf chain-rank repair~~ — fully
resolved in 1.2.3.  The struct-vs-bytes bloom-header fix
(1.2.2) plus the pending-overlay positional-filter fix
(1.2.3) close out the long-running tier-3 bypass.  Tier-3
is on by default and works correctly across single-leaf,
multi-leaf, and pending-overlay code paths.  Variable-width
blooms are now an incremental size optimization on top of
working tier-3.

## Problem

The tier-3 per-tuple bloom is a fixed `pg_tre.bloom_tuple_bits =
128` bits per heap row, sized to roughly hold one row's distinct
trigrams at a tolerable false-positive rate.  Two issues:

1. **Wasteful for short rows.**  A row with 5 distinct trigrams
   needs <8 bits to keep FP rate below 1%; we burn 128.
2. **Saturated for long rows.**  A row with 200 distinct trigrams
   in a 128-bit bloom has FP rate ≈ 1.0 — the bloom is useless,
   and tier-3 still has to read and check it.

Both extremes pay full cost in storage and in the per-row
recheck phase.

## Proposal

Encode the bloom width per row, sized from the row's distinct
trigram count at index-build time.  Three categories:

| Row trigram count `k` | Storage | FP rate target |
|---|---|---|
| `k = 0` | flag bit only, no bloom | n/a |
| `1 ≤ k ≤ 32` | 16-bit bloom + 4-bit hash count | <0.1% |
| `33 ≤ k ≤ 256` | 64-bit bloom + 5-bit hash count | <1% |
| `k > 256` | flag bit "always passes" + no bloom | accept the recheck cost |

Width is decided once at build time per row.  Stored in the
posting-leaf payload as:

```
struct PerRowBloom {
    uint8 width_class;   // 0=empty, 1=16, 2=64, 3=always-pass
    uint8 nhashes;       // hash function count
    union {
        uint16 b16;
        uint64 b64;
        // width_class=0 or 3 carries no payload
    } bits;
}
```

Average payload:

| Width class | Bytes per row |
|---|---|
| 0 | 1 |
| 1 | 4 (1 + 1 + 2 padded to 4) |
| 2 | 16 (1 + 1 + 8 padded to 16) |
| 3 | 1 |

For a corpus where most rows have <32 distinct trigrams (typical
for code identifiers, log lines, SKUs), payload averages ≈4-6
bytes per row vs 16 today.  **70-80% payload reduction.**

For a corpus of long prose (paragraphs, articles), most rows
land in width_class 2 or 3 — comparable to today's flat 128-bit,
or smaller.

## Read path

`pg_tre_posting_lookup_tuple_bloom` reads `width_class` first,
then dispatches:

- 0: row matches every query trigram trivially (unlikely; only
  empty rows).
- 1, 2: dispatch to width-specific bloom check using the right
  `nhashes` value.
- 3: skip the bloom check, return "candidate, must recheck".

## Why this depends on chain-rank repair

The current chain-rank lookup (in
`pg_tre_posting_lookup_tuple_bloom`) is broken for multi-leaf
posting trees: the per-row payload offset is computed from a
per-leaf rank that doesn't accumulate across right-link chains.
Today this is bypassed entirely via
`pg_tre.tuple_bloom_enable=false`.

Fixing chain-rank means:

1. The chain walker maintains a running TID-count as it
   traverses leaves.
2. `sm_rank` on a per-leaf basis returns the local rank; the
   walker adds it to the running count.
3. The payload offset table (currently per-leaf) becomes a
   per-leaf-with-base offset, indexed by the global rank.

Once chain-rank works, switching tier-3 default back to `true`
becomes safe.  At that point variable-width blooms become a
size optimization on top of a working tier-3.

## On-disk format compatibility

Adding a `width_class` byte changes the per-row payload layout.
Two options:

**Option A (simpler):**  Bump `PG_TRE_FORMAT_VERSION` to 4.
Existing 1.x indexes need REINDEX.  Easy to test, simple
upgrade-tests matrix update.

**Option B (compatible):**  Encode width_class in the existing
payload region by reserving a sentinel value.  Existing
fixed-128-bit blooms continue to read with the old code path;
new builds emit variable-width.  No format bump.

Option B requires careful sentinel-value choice and audit; Option
A is cleaner.  Since posting-page coalescing (v2.0) is also a
format bump, this work might bundle with that and ship in v2.0
instead of v1.3.  Decision deferred until chain-rank repair lands
and we measure the actual size improvement on a real corpus.

## Implementation phases

1. **Phase 1 — chain-rank repair.**  `pg_tre.tuple_bloom_enable`
   becomes safe to set true; default still false.  Regression
   tests for the multi-leaf 'the' case (currently masked).
2. **Phase 2 — width_class encode/decode.**  Build path picks
   width based on row's distinct trigram count.  Read path
   dispatches.  No size change yet (still emits 16-byte
   payload, just with a width tag).
3. **Phase 3 — variable storage.**  Width-1/2 actually emit 4 /
   16-byte payloads.  Format-version bump or sentinel
   compatibility shim per the decision above.
4. **Phase 4 — re-default tier-3 to true.**  Once chain-rank
   and variable-width are correct and measured, flip
   `pg_tre.tuple_bloom_enable` default back to true.

## Risks

- **The chain-rank repair is non-trivial.**  Touches the scan
  path's hottest loop (per-row tier-3 lookup).  Needs a
  benchmark before/after; risk of slowdown on the unaffected
  single-leaf case if the chain accumulator adds branches.
- **Build CPU cost.**  Counting distinct trigrams per row is
  free at build time (we already accumulate them); width
  selection is one comparison.  Negligible.
- **Adversarial inputs.**  A row with crafted trigram count
  exactly at the width-class boundary (32, 256) could cause
  thrashing on rebuild after VACUUM removes tids; pin the
  width on the highest historical count to avoid oscillation.
