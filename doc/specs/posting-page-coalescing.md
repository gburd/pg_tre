# Posting-page coalescing — v2.0 design spec

**Status:** draft, targeting pg_tre 2.0 (on-disk format bump).

**Owner:** unassigned.

## Problem

The dominant size cost of a pg_tre index today is structural:
**one 8 KB posting-tree page per distinct trigram**, even for
trigrams that map to a single TID.  Measured on a 10K-row corpus
of short text:

- pg_tre  37 MB / 4708 pages  (~4696 unique trigrams ≈ 1 page each)
- pg_trgm  2.6 MB / 330 pages

The 14× gap is almost entirely "page count", not bloom-filter
overhead.  Toggling `pg_tre.tuple_bloom_enable` between true and
false changes the size by less than 1%.

The 1.2.1 inline-threshold tuning (256 → 384 bytes) helps the
smallest trigrams stay in the upper tree, but a trigram with
~50 TIDs still gets its own posting page even though the actual
serialized sparsemap is ~400 bytes — the page is 95% empty space.

## Proposal

Add a new on-disk page kind, `PG_TRE_PAGE_POSTING_COALESCED`,
that stores the postings for **multiple trigrams** packed
contiguously on a single page, with an indirection table at the
top of the page mapping `(trigram_hash → offset, length)`.

The upper-tree leaf entry for a coalesced trigram becomes:

    {
        block_number    page;          // physical page
        uint16          slot_idx;      // index into the page's
                                       // indirection table
    }

instead of a single block_number for a one-trigram-per-page
posting.

### Decision: when to coalesce

Build path computes the serialized sparsemap size for each
trigram.  Bucket by size:

| Size bucket | Storage |
|---|---|
| < 384 bytes | inline in upper-tree leaf (already implemented) |
| 384 – 4096 bytes | **coalesced page** — pack 4-20 trigrams per 8 KB page |
| > 4096 bytes | dedicated posting tree (existing path) |

Coalesced pages are 95% utilized at build time.  Bin-packing
algorithm: greedy first-fit on a sorted-by-size list of
trigrams.  Quantize sizes to 64-byte boundaries to cap the
indirection-table search space.

Sustained-write workload: trigrams in the pending list flush via
the existing posting-tree code path.  Coalesced pages are **build-
only**; ongoing writes to a coalesced trigram migrate it out to
its own posting tree (the page-kind distinction lives in the
upper tree, so the trigram's storage type can change between
builds).

### Page layout

```
┌─────────────────────────────────────────────────────────────┐
│ PageHeaderData (24 bytes)                                   │
├─────────────────────────────────────────────────────────────┤
│ PageTreOpaque trailer:                                      │
│     format_version, kind = PG_TRE_PAGE_POSTING_COALESCED    │
│ (after PG's special-area pointer)                           │
├─────────────────────────────────────────────────────────────┤
│ Page-local header:                                          │
│     uint16 n_slots                                          │
│     uint16 free_offset                                      │
│     uint8  pad[12]                                          │
├─────────────────────────────────────────────────────────────┤
│ Indirection table (n_slots entries, 16 bytes each):        │
│     uint64 trigram_hash                                     │
│     uint16 sm_offset                                        │
│     uint16 sm_length                                        │
│     uint16 payload_offset    (0 if no payload)              │
│     uint16 payload_length                                   │
├─────────────────────────────────────────────────────────────┤
│ Sparsemap blobs (variable, 384–4096 bytes each)             │
│ Payload blobs (per-tuple blooms, positions)                 │
│ ...                                                         │
└─────────────────────────────────────────────────────────────┘
```

### Read path

`pg_tre_posting_materialize` switches on the upper-tree leaf
entry kind:

- inline: sparsemap follows the leaf entry (existing path).
- dedicated posting tree (`PG_TRE_PAGE_POSTING_L`): walk
  right-link chain (existing path).
- **coalesced (new)**: read the page, look up `trigram_hash` in
  the indirection table via a 16-byte-stride linear scan
  (cache-friendly), copy the sparsemap blob into a fresh
  sm_open_copy buffer.

### Vacuum

`pg_tre_amvacuumcleanup` already rebuilds posting trees that lose
their last TID.  For coalesced pages: when a trigram's TIDs all
become dead, mark the slot `INVALID` (offset = UINT16_MAX) but
don't reclaim space until the page falls below 50% utilization,
at which point we rewrite the page (compacting and removing
INVALID slots).

## On-disk format version

Bump `PG_TRE_FORMAT_VERSION` from 3 to 4.  Existing 1.x indexes
are unreadable by 2.0 and require REINDEX.  Document in
RELEASING.md's compatibility matrix.  Add 1.0.0–1.2.x to the
upgrade-tests `exclude:` list.

## Expected impact

For the 10K-row fixture above, with average trigram size ≈ 50
bytes and ≈ 12 trigrams per coalesced page:

- 4696 trigrams ÷ 12/page = **390 coalesced pages** (vs 4696
  posting pages today)
- Plus ~50 dedicated-posting-tree pages for the long tail of
  high-cardinality trigrams.
- Total: ~440 pages vs 4708 today, **~10× page-count reduction**.

Index size drops from 37 MB to ~3.5 MB — putting pg_tre roughly
at parity with pg_trgm for sparse-trigram corpora.

## Risks

- **WAL volume**: coalesced-page mutations carry full-page
  images today (we set REGBUF_FORCE_IMAGE everywhere — see
  CHANGELOG 1.2.1 fixed list).  Multi-trigram pages mean each
  WAL record covers more state per byte, which is good, but
  delta-aware redo (a separate v1.3 followup) is what makes
  this efficient.  Without delta redo, every coalesced-page
  mutation ships an 8 KB FPI.
- **Hot trigrams + coalescing**: a trigram that grows past the
  page budget needs to migrate out to its own posting tree.
  The migration is a transactional no-op for queries (upper-tree
  pointer atomically swaps from coalesced-slot to posting-tree-
  root) but adds bookkeeping complexity to amvacuumcleanup and
  aminsert.

## Implementation phases

1. **Phase 1 — page layout and read path.**  Add the new page
   kind, indirection-table accessors, materialize() switch.
   Build path emits coalesced pages but reads still work.
   Tests: pageinspect-style verification + existing regression
   suite (no behavior change).
2. **Phase 2 — bin packing in build.**  Sort trigrams by size
   at build time, first-fit-decreasing into 8 KB pages.
   Measure size impact on the standard fixture.
3. **Phase 3 — write path migration.**  Hot-trigram migration
   from coalesced → dedicated posting tree on growth past the
   page budget.  Tested via stress.sh with high write churn.
4. **Phase 4 — vacuum and reclaim.**  INVALID slot tombstones,
   page-rewrite when utilization drops below 50%.

Each phase ends in a runnable, regression-clean state with a
metric to compare against pg_trgm.

## Out of scope

- Inter-page compression (zstd / LZ4 across multiple coalesced
  pages).  Possible v3.0; deferred to keep page-level reads
  random-access friendly.
- BRIN-style summary blooms over coalesced pages (every page
  gets a "what trigrams live here" bloom for early skip).
  Possible v3.0.
