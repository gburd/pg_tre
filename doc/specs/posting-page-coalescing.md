# Posting-page coalescing - design spec (format v7 → v8)

> **North star: pg_tre is a REGEX index with edit distance.**
> Coalescing changes the on-disk *layout and size* of postings,
> never *what the index matches*.  The executor recheck contract is
> untouched: every emitted row is still rechecked by the real
> operator.  A coalesced index returns byte-identical query results
> to a non-coalesced one.

**Status:** Phase 1 (page kind + read path + additive format bump)
landing; Phases 2-4 specified, not yet built.  **Target:** 2.0 line
(additive on-disk format change v7 → v8, **no REINDEX**).

## Problem

The dominant size cost of a pg_tre index today is structural:
**one 8 KB posting-tree page per distinct trigram**, even for
trigrams whose serialized sparsemap is a few hundred bytes.
Measured on a 10K-row corpus of short text:

- pg_tre  37 MB / 4708 pages  (~4696 unique trigrams ≈ 1 page each)
- pg_trgm  2.6 MB / 330 pages

The ~14× gap is almost entirely *page count*, not bloom-filter
overhead (toggling `pg_tre.tuple_bloom_enable` moves the size by
< 1%).

### What the existing inline optimization already covers

`PG_TRE_INLINE_POSTING_MAX` (currently **2048 bytes**, see
`include/pg_tre/posting.h`) keeps the *smallest* postings out of
dedicated pages: a posting whose serialized sparsemap (+ payload)
fits in 2048 bytes lives **inline in the upper-tree leaf entry**,
packed after the `PgTreUpperLeafEntry` array.  Several such inline
blobs share one `PG_TRE_PAGE_UPPER_L` page, so the single-TID and
tiny-posting case is already dense.

The inline ceiling cannot be lifted past ~2048-3072 without
breaking the upper tree: an oversized inline entry leaves too few
entries per `UPPER_L` leaf for the internal-level layout to address
(see the `PG_TRE_INLINE_POSTING_MAX` history note).  So the gap
that remains is the **medium bucket**: a trigram whose posting is
2 KB-~7.6 KB (the single-leaf posting budget) gets a *dedicated
8 KB posting leaf* that is mostly empty space.  On the 10K-row
fixture the long tail of "~50 TID" trigrams that serialize to a
few hundred bytes but exceed 2048 once payload (per-tuple blooms +
positions) is attached land here, one wasteful page each.

> **Why not just raise the inline threshold?**  Because the inline
> blob shares the `UPPER_L` leaf, and the upper tree's internal
> level can only address a bounded number of leaves; large inline
> blobs starve it.  Coalescing puts the medium postings on their
> *own* dedicated page kind that the upper tree references by block
> (exactly like a posting root today), so the upper-tree fan-out is
> unaffected.

## Proposal

Add a new on-disk page kind, **`PG_TRE_PAGE_POSTING_COALESCED`**
(tag 9, format v8), that stores the postings of **multiple
trigrams** packed contiguously on one page, with an indirection
table at the start of the content area mapping a slot index to
`(sparsemap_offset, sparsemap_length, payload_offset, payload_length)`.

The upper-tree leaf entry already records `posting_root` (a block
number) and `inline_bytes` (a uint32, zero for the out-of-line
case).  A coalesced trigram reuses **both fields additively**:

```
posting_root  := the coalesced page's block number
inline_bytes  := PG_TRE_COALESCED_FLAG | slot_idx
                 (high bit set ⇒ "posting_root is a coalesced page,
                  low 16 bits are the slot index on that page")
```

`PG_TRE_COALESCED_FLAG == 0x80000000`.  A v6/v7 index **never**
sets this bit (inline blobs are < 2048 bytes, so `inline_bytes <
2048` always, and a non-inline entry has `inline_bytes == 0`).  So
the `PgTreUpperLeafEntry` struct is byte-identical to v7 - the bump
is **purely additive**.  See "On-disk format version" below.

### Resolving an upper-tree leaf entry (read path)

`pg_tre_upper_lookup_root` fills a `PgTreUpperRef`.  After this
change it classifies the entry into three cases:

| Entry shape | `out->root` | `out->inline_data` | meaning |
|---|---|---|---|
| `inline_bytes ∈ (0, 2048)` | Invalid | ptr into leaf | inline (existing) |
| `inline_bytes == 0` | `posting_root` | NULL | dedicated posting tree (existing) |
| `inline_bytes & 0x80000000` | `posting_root` (coalesced page) | NULL | **coalesced** (new) |

To keep the rest of the read path untouched, the lookup resolves
the coalesced slot eagerly into the `PgTreUpperRef`'s existing
`inline_data`/`inline_bytes` shape: it reads the coalesced page,
finds the slot's sparsemap blob, and hands back a *copy* of that
blob as `inline_data` (with `root = InvalidBlockNumber`).  Every
downstream consumer - `pg_tre_posting_materialize`,
`pg_tre_posting_scan_begin`, `pg_tre_posting_cardinality`,
`pg_tre_posting_lookup_*` - then sees the coalesced posting as if
it were an ordinary inline blob, with **zero changes to those
functions**.  The copy is palloc'd in the caller's context and
freed via `pg_tre_upper_release` (which gains a "free the resolved
copy if owned" step).

> Rationale for resolve-to-inline vs. a fourth posting source:
> the four posting consumers all already branch on
> `inline_data != NULL`.  Mapping coalesced → inline at the single
> lookup chokepoint means the feature is **one new branch in one
> function** on the read side, not four parallel implementations.
> The cost is one extra ~hundreds-of-bytes memcpy per resolved
> coalesced trigram per scan - negligible next to the B-tree
> descent it rides on, and the slot blob is exactly the bytes the
> consumer would have copied out of a dedicated leaf anyway.

### Coalesced page layout

> **2.0.2 correctness fix — two-ended writer.**  The 2.0 writer grew
> the indirection table AND the blobs both upward from the header,
> placing slot 0's blob at the very address (`&slots[1]`) that the
> NEXT slot's 16-byte table entry would later occupy.  Adding slot 1
> then clobbered the head of slot 0's blob, so slot 0 resolved to a
> corrupt sparsemap — a **false negative** (an AND of trigrams in
> adjacent slots intersected to empty, returning 0 rows).  Latent in
> 2.0 because pages rarely held >1 slot; surfaced once the band
> widened.  Fixed by a **two-ended (heap-page-style) layout**: the
> table grows UP (`pd_lower` = end of table), blobs grow DOWN from
> the end of the usable area (`pd_upper` = lowest blob start), with
> the free hole between them.  `REGBUF_STANDARD` strips only the
> hole, so both regions survive the FPI.  Readers are unaffected —
> `pg_tre_coalesced_resolve_slot` uses the slot's absolute
> `sm_offset`/`sm_length`, which are layout-direction agnostic.

```
┌───────────────────────────────────────────────────────────────┐
│ PageHeaderData                                       24 B      │
├───────────────────────────────────────────────────────────────┤
│ PgTreCoalescedHeader  (content area start):                   │
│     uint16 n_slots                                            │
│     uint16 free_offset   (low edge of blob region, from start)│
│     uint32 _pad0                                              │
├───────────────────────────────────────────────────────────────┤
│ Indirection table: n_slots × PgTreCoalescedSlot (16 B each):  │
│     uint16 sm_offset       (from page start; 0 ⇒ INVALID)     │
│     uint16 sm_length                                          │
│     uint16 payload_offset  (from page start; 0 ⇒ no payload)  │
│     uint16 payload_length                                     │
│     uint64 trigram_hash    (self-describing / verify on read) │
│   (table grows UP; pd_lower = end of table)                   │
├───────────────────────────────────────────────────────────────┤
│ ... free hole (stripped by REGBUF_STANDARD) ...               │
├───────────────────────────────────────────────────────────────┤
│ Packed blobs (grow DOWN from the end of usable area):         │
│     ... slot 1 sparsemap / payload ...                        │
│     slot 0 sparsemap / payload                                │
│   (free_offset = pd_upper = lowest blob start)                │
├───────────────────────────────────────────────────────────────┤
│ PageTreOpaqueData (page_kind = COALESCED, version = 8)  8 B   │
└───────────────────────────────────────────────────────────────┘
```

Two-ended layout (2.0.2; see the correctness-fix note below): the
indirection table grows UP from the header and `pd_lower` is set past
it; the packed blobs grow DOWN from the end of the usable content
area and `pd_upper` (== the header's `free_offset`) is set to the
lowest blob.  The free hole sits between `pd_lower` and `pd_upper`,
and `REGBUF_STANDARD` strips only that hole, so the table (below
`pd_lower`) and the blobs (at/above `pd_upper`) both survive FPI
hole-stripping.  Slot offsets are absolute (`from page start`), so
the read path is independent of the growth direction.

`trigram_hash` in each slot is verification-only: the upper-tree
entry is authoritative for which slot belongs to which trigram, but
the slot carries the hash so a corrupt/torn page is caught
(`slot.trigram_hash != entry hash ⇒ ERROR data_corrupted`) rather
than returning a foreign trigram's TIDs.

A coalesced page holds **multiple trigrams' postings**.  An
indirection slot resolves in O(1) (`slots[slot_idx]`); the
`slot_idx` comes straight from the upper-tree entry, so no scan of
the table is needed on read.

### Decision: when to coalesce (build path)

Build computes the serialized sparsemap (+ payload) size for each
trigram, then buckets:

| Size (sparsemap + payload) | Storage |
|---|---|
| ≤ 2048 bytes (`PG_TRE_INLINE_POSTING_MAX`) | inline in upper leaf (existing) |
| 2048 B - `PG_TRE_COALESCE_MAX` (~3 KB) | **coalesced page** (new) |
| > `PG_TRE_COALESCE_MAX` | dedicated posting tree / chain (existing) |

`PG_TRE_COALESCE_MAX` caps a single slot so a coalesced page packs
at least ~2-3 trigrams (a page that fits one trigram is no better
than today's dedicated leaf).  Coalesced-page packing is **greedy
first-fit-decreasing** over the medium-bucket trigrams sorted by
size: open a page, append slots until the next blob would overflow
the page budget, then start a new page.

> **2.0.2 update — widened band.**  `PG_TRE_COALESCE_MAX` was a fixed
> 3072 in 2.0, which only ever packed ~2 slots and left the entire
> `(3072, single-leaf-budget]` band (the bulk of medium/high-
> cardinality trigrams on real corpora) in dedicated one-page-each
> leaves — the 60.9 KB/row density blocker.  It is now a *derived*
> value, `pg_tre_coalesce_max()` = the largest sparsemap for which
> **at least two slots fit on a page** (`2 * (sizeof(slot) +
> MAXALIGN(cap)) <= budget`, ≈ 4056 B today).  The band is now
> `(2048, ~4056]`; postings in it pack 2-3 per coalesced page via the
> existing streaming writer.  Above the cap a dedicated leaf is the
> right call: it is one page either way *and* keeps its per-tuple
> payload, which a coalesced slot drops.  No on-disk struct changed
> (same v8 page kind / slot struct); this is a build-time packing
> *policy* change, still gated by `pg_tre.coalesce_enable`.  Measured
> 1.2x-1.6x index-size reduction on corpora with a medium-cardinality
> trigram tail.

Coalesced pages are **build-only** (and merge-only - a Hanoi merge
that rebuilds an upper tree may emit them).  Ongoing single-row
inserts go through the existing pending-list → posting-tree path; a
coalesced trigram that grows on write **migrates out** to its own
posting tree (Phase 3).  The page-kind distinction lives entirely
in the upper-tree entry, so a trigram's storage class can change
between builds without touching the coalesced page.

### Vacuum

`pg_tre_amvacuumcleanup` rebuilds/repacks posting trees that lose
TIDs.  For coalesced pages (Phase 4): when a slot's TIDs all become
dead, mark the slot INVALID (`sm_offset = 0`) and leave the bytes;
when a page falls below 50% live utilization, rewrite it
(compacting live slots, dropping INVALID ones) and re-point the
surviving trigrams' upper-tree entries at the new slots.  Until
Phase 4, a coalesced page is rebuilt wholesale by REINDEX / a full
rebuild; VACUUM treats coalesced slots as read-only (it can strip
dead TIDs from the *inline-resolved* copy on scan, which is lossy-
safe because recheck is authoritative, but does not rewrite the
page).  **Phase 1 ships read + build only**; coalesced pages are
produced only by a from-scratch build and never mutated in place,
so the VACUUM interaction is "leave them alone," which is correct
(stale-but-superset postings are filtered by recheck).

**Maintenance-path safety (landed alongside the build/read path).**
"Leave them alone" is not automatic: a coalesced upper-tree entry
stores `posting_root = coalesced-page block` and `inline_bytes =
PG_TRE_COALESCED_FLAG | slot`, so every maintenance routine that
treats `posting_root` as a posting-tree root or `inline_bytes` as a
blob length must special-case it or it corrupts.  Three such paths
are now coalesced-aware (each skips or resolves the marker instead
of misreading it):

- `ambulkdelete` / `posting_upper_walk` - skips coalesced entries
  (does not walk the coalesced page as a posting tree).
- `posting_leaf_inline_delete` - skips coalesced entries (does not
  read `flag | slot` as an inline-blob length).
- `snapshot_upper_leaf` (merge / Hanoi / collapse) - preserves the
  coalesced entry verbatim and does not consume from the inline
  blob region; `collapse_fold_run` resolves the slot via
  `pg_tre_coalesced_resolve_slot` before materializing.

Result: with `pg_tre.coalesce_enable = on`, VACUUM, INSERT-driven
merges, and Hanoi/collapse are all correct (verified by
`coalesce_vacuum` regression and the full-corpus nightly).  Dead
TIDs inside a coalesced posting are still not space-reclaimed (they
are recheck-filtered, and a merge that touches the trigram rebuilds
it as a dedicated leaf); in-place coalesced-page compaction is the
remaining Phase 4 work.  `tre_coalesced_page_count(regclass)`
exposes how many coalesced pages an index holds.

## On-disk format version - additive, NO REINDEX

```
PG_TRE_FORMAT_VERSION_LATEST = 8   (was 7)
PG_TRE_FORMAT_VERSION_MIN    = 6   (unchanged - v6 and v7 still read)
```

The bump is **additive**, mirroring the v6 → v7 run-catalog bump
(`doc/specs/phaseB1-run-catalog.md`):

- **No existing page kind changes layout.**  `PgTreUpperLeafEntry`,
  posting leaves, range pages, the meta page - all byte-identical
  to v7.  A v6/v7 index has no coalesced pages and never sets
  `PG_TRE_COALESCED_FLAG`, so it reads unchanged under v8 code.
- **The new page kind only appears in v8 indexes.**  A v8 index
  built from scratch (or rebuilt by a future merge) may contain
  `PG_TRE_PAGE_POSTING_COALESCED` pages stamped format_version 8.
  v8 readers handle them; the `[MIN, LATEST]` range check in
  `pg_tre_read` (`src/pages/buffer.c`) accepts v8.
- **`PG_TRE_FORMAT_VERSION_MIN` stays 6.**  v6 and v7 indexes are
  read in place with **no REINDEX**.  This is the project's
  strongly-preferred posture (contrast the v5 → v6 sparsemap break,
  which forced a painful production REINDEX).
- **`pg_tre_upgrade_index` stamps v8** on every page in place; the
  per-page bytes are unchanged (v6/v7/v8 non-coalesced pages are
  byte-identical), so the upgrade is the same no-op-rewrite +
  version-stamp it already is.  An upgraded index gains no
  coalesced pages - those only arrive on the next full rebuild -
  but it is correctly *labelled* v8 and a from-scratch v8 build of
  the same table is dense.

> Why a version bump at all, if old pages don't change?  Because a
> v8 index *may* contain a page kind (`COALESCED`) that v7 code
> would reject as `expected_kind` mismatch / unknown kind.  The bump
> lets a v7 binary refuse to open a v8 index it cannot fully read,
> rather than silently mis-resolving a coalesced slot.  The MIN
> staying at 6 is what guarantees forward readers accept old
> indexes; the LATEST bump is what guarantees old readers reject
> *new* indexes.

## WAL / crash recovery

A coalesced page is WAL-logged exactly like every other pg_tre page
that ships full-page images, following the **run-catalog writer
pattern** (`pg_tre_run_catalog_append`, the validated reference):

- Register the page with `REGBUF_FORCE_IMAGE | REGBUF_STANDARD`.
  **Never `REGBUF_WILL_INIT`** - that implies `NO_IMAGE` and PANICs
  the generic `pg_tre_redo_fpi` (a real bug we already fixed once).
  `pg_tre_extend` physically extends the relation, so the block
  exists at replay and the generic FPI redo restores the image with
  no new redo routine.
- All page edits, `MarkBufferDirty`, `XLogInsert`, and `PageSetLSN`
  inside one `START_CRIT_SECTION()/END_CRIT_SECTION()`.
- Reuse the existing `XLOG_PTRE_POSTING_INSERT` op code (it already
  routes through `pg_tre_redo_fpi`); no new rmgr op code, no new
  redo routine, no `pg_tre_identify`/`pg_tre_desc` change.  Build
  emits one record per coalesced page (a coalesced page is written
  once, fully, at build time - the same "write whole page once"
  shape as a posting leaf).

Standby replay, consistency masking (`pg_tre_mask`), and the
WAL-audit differential need no changes: a coalesced page masks like
any other (LSN/checksum/hint-bits/hole), and its structural bytes
(header + indirection table + blobs, all below `pd_lower`/within
`[pd_lower, pd_upper)`) must match primary↔standby byte-for-byte,
which is the correct invariant.

## Expected impact

For the 10K-row fixture, with the medium bucket (~hundreds of
trigrams at a few hundred → ~3 KB each) packed ~3-8 per coalesced
page instead of one dedicated leaf each, the dedicated-posting page
count for that bucket drops by 3-8×.  The exact index-size delta is
measured in the regression (`size` test, Phase 2), not asserted as
a fixed number here - the win is real but corpus-dependent, and the
test compares page counts before/after rather than hard-coding a
ratio.

## Implementation phases

1. **Phase 1 - page kind + format bump + read path (THIS increment).**
   - `PG_TRE_PAGE_POSTING_COALESCED` page kind, `PgTreCoalescedHeader`
     + `PgTreCoalescedSlot` structs, `PG_TRE_COALESCED_FLAG`.
   - Format bump v7 → v8 (additive; MIN stays 6).
   - `pg_tre_upgrade_index` / `pg_tre_upgrade_page_to_latest`
     accept v7 and stamp v8.
   - Coalesced-page **writer** (`pg_tre_coalesced_write_page`,
     run-catalog WAL pattern) and **reader/resolver**
     (`pg_tre_coalesced_resolve_slot`) - the lookup chokepoint maps
     a coalesced entry to inline bytes.
   - `pg_tre_upper_lookup_root` recognizes the coalesced flag.
   - Tests: round-trip a coalesced page (write slots, resolve each,
     assert TIDs match); v7→v8 upgrade no-REINDEX; from-scratch v8
     build returns identical query results.
   - **Build path still emits dedicated leaves by default** - Phase 1
     wires the machinery and proves it round-trips, behind a GUC
     (`pg_tre.coalesce_enable`, default off) so the default build is
     byte-for-byte v7-equivalent and the regression suite is
     unchanged.  This keeps Phase 1 reviewable and revertible: the
     new code paths exist and are tested, but the size change does
     not land until Phase 2 flips the default and measures it.
2. **Phase 2 - flip default + size regression.**  Enable by default
   and add a `size` regression comparing coalesced-page counts to the
   dedicated-leaf baseline.
   - **First-fit-decreasing bin packing was evaluated and skipped.**
     The medium bucket is `(2048, 3072]` bytes, so a coalesced page
     holds only ~2-3 slots (8160-byte budget / ~2600-byte slot).
     At 2-3 items per bin the FFD gain over the streaming greedy
     writer is negligible, and FFD would require buffering the whole
     medium bucket before packing.  The greedy next-fit writer is
     near-optimal at this fanout; revisit only if the bucket bounds
     widen.  The real coalescing win is *coalesced page vs dedicated
     leaf* (a ~3× reduction for the medium bucket: ~3 postings share
     one 8 KB page instead of wasting one each), not slot packing.
   - **Flipping the default is deferred to the operator** (this work
     keeps `pg_tre.coalesce_enable` off); the capability is complete
     and maintenance-safe (below), ready to un-gate.
3. **Phase 3 - write-path migration.**  A coalesced trigram that
   grows past its slot on insert migrates to a dedicated posting
   tree (upper-tree entry atomically swaps coalesced-slot → root).
   - **Already handled implicitly by the merge path:** a pending
     merge that touches a coalesced trigram materializes its existing
     posting (resolving the slot), unions the new TIDs, and rebuilds
     it as a *dedicated* leaf - the merge never re-coalesces.  The
     orphaned coalesced slot leaks until a full rebuild (Phase 4
     reclaim).  No in-place slot-growth migration is needed.
4. **Phase 4 - vacuum reclaim.**  INVALID-slot tombstones; rewrite a
   coalesced page when live utilization drops below 50%.
   - **First increment landed:** the maintenance paths
     (`ambulkdelete`, `posting_leaf_inline_delete`, merge snapshot)
     are now coalesced-aware and correct - VACUUM no longer corrupts
     on coalesced entries.  Remaining: in-place compaction to
     *reclaim* the space of dead/orphaned coalesced slots.

## Out of scope

- Inter-page compression (zstd/LZ4 across coalesced pages) - keeps
  page reads random-access friendly; possible v3.0.
- Per-page summary blooms ("what trigrams live here") for early
  skip - possible v3.0.
