# pg_tre on-disk page format (v3, v4)

Authoritative declarations live in `include/pg_tre/page.h`.  This
document is the narrative reference and the place where format
changes are reviewed before they are locked in.

All multi-byte fields use native byte order.  All pages are the
Postgres cluster page size (default 8 KiB) and begin with the
standard `PageHeaderData` at offset 0 and end with a
`PageTreOpaqueData` trailer in the special area.

## Format version history

- **v8 (2.0.0-dev)**: Posting-page coalescing.  Adds a new page kind
  `PG_TRE_PAGE_POSTING_COALESCED` that packs the postings of multiple
  trigrams onto one page, addressed by a slot index carried in the
  upper-tree leaf entry's `inline_bytes` field (high bit
  `PG_TRE_COALESCED_FLAG`).  Purely ADDITIVE: no existing page kind
  changes layout, `PG_TRE_FORMAT_VERSION_MIN` stays 6, and a v6/v7
  index never sets the coalesced flag, so it reads unchanged with NO
  REINDEX.  The new page kind appears only in indexes built or
  rebuilt at v8.  See `doc/specs/posting-page-coalescing.md`.
- **v7 (2.0.0-dev)**: Run/level catalog (Phase B1).  Adds the
  `PG_TRE_PAGE_RUN_CATALOG` page kind and meta-page run fields.
  Additive: a v6 index reads as one implicit run, no REINDEX.
- **v6 (1.6.0)**: Sparsemap 4.0.0 wire format (64-bit chunk offsets).
  BREAKING -- v<6 indexes require REINDEX (the old 32-bit format is
  not backward-readable and silently lost data on large heaps).
- **v5 (Phase 8)**: Multi-leaf range tier (`PgTreRangeHeader` +
  `right_link` chain).  Only RANGE pages differ from v4; readers
  handle v<5 range pages for back-compat.
- **v4 (1.4.0-dev)**: Identical byte layout to v3; the bump exists
  to land the in-place format-upgrade infrastructure (see below).
  No reader change between v3 and v4 beyond accepting both at
  page-decode time.
- **v3 (Phase 4.2)**: Multi-leaf posting trees. When a single trigram's
  posting exceeds ~8 KB, the builder splits it across multiple leaves
  linked by `right_link` (Lehman-Yao convention). Each leaf stores
  `min_tid` and `max_tid` bounds. Readers traverse the chain to find
  the leaf containing a target TID.
- **v2 (Phase 3.5)**: Codepoint-based trigrams.
- **v1**: Initial format with byte-based trigrams.

Indexes built with v2 or earlier require REINDEX to use 1.x.
Indexes built with v3 are read directly by 1.4.0-dev; in-place
upgrade to v4 is available via `pg_tre_upgrade_index()`.

## Meta page (block 0)

    +-----------------------------------+
    | PageHeaderData                    |  24 B
    +-----------------------------------+
    | PgTreMetaPageData                 |  variable (reserved[32])
    |   uint32  magic          0x50545245  ('PTRE')
    |   uint32  format_version 1
    |   uint32  q                        trigram size (default 3)
    |   uint32  tri_encoding             0=byte, 1=codepoint-hash
    |   uint32  bloom_range_m_bits
    |   uint16  bloom_range_k
    |   uint16  bloom_tuple_m_bits
    |   uint8   bloom_tuple_k
    |   uint8   _pad[3]
    |   Block   root_upper
    |   Block   root_range
    |   Block   pending_head
    |   Block   pending_tail
    |   uint64  pending_n_entries
    |   Block   stats_blk
    |   uint64  n_trigrams
    |   uint64  n_tuples_indexed
    |   uint32  created_xid
    |   uint32  reserved[32]
    +-----------------------------------+
    |                                   |  (unused)
    +-----------------------------------+
    | PageTreOpaqueData                 |   8 B
    |   page_kind = META                |
    +-----------------------------------+

## Upper tree pages

Internal node (UPPER) and leaf node (UPPER_L) share the same header
layout but carry different entry types.

    internal: (trigram_hash_low_watermark, child_blk) pairs
    leaf:     PgTreUpperLeafEntry[] sorted by trigram_hash

Split protocol: Lehman-Yao right-link.  Right-link is stored in the
opaque area's flags region.

## Posting tree pages

Internal (POSTING) and leaf (POSTING_L) pages.

### POSTING (internal)

    (min_tid, child_blk) pairs, sorted by min_tid.

### POSTING_L (leaf)

Phase 4.2: supports multi-leaf chains when a single trigram's posting
exceeds the ~8 KB single-leaf budget. Leaves are linked via `right_link`
(Lehman-Yao style). Each leaf stores its TID range in `min_tid` and
`max_tid`. Readers traverse right-links to find the leaf containing a
target TID.

    +-----------------------------------+
    | PageHeaderData                    |  24 B
    +-----------------------------------+
    | PgTrePostingLeafHeader            |  40 B
    |   right_link                      |  next leaf in chain (or InvalidBlockNumber)
    |   min_tid / max_tid               |  TID range for this leaf
    |   sparsemap_bytes                 |
    |   payload_bytes                   |
    |   payload_offset                  |
    |   n_entries                       |
    +-----------------------------------+
    | sparsemap blob                    |  sparsemap_bytes
    +-----------------------------------+
    |                                   |
    |   (free space)                    |
    |                                   |
    +-----------------------------------+
    | payload region (grows from below) |  payload_bytes
    |   per-TID: (pos_list_varlen,      |
    |             tuple_bloom_128)      |
    +-----------------------------------+
    | PageTreOpaqueData                 |   8 B
    |   page_kind = POSTING_L           |
    +-----------------------------------+

Access by TID:
1. Walk right-links until `min_tid <= target <= max_tid`
2. Within that leaf: `sparsemap_rank(map, 0, TID, true)` gives the entry
   index; multiply by sizeof(payload record) -- or walk the variable-
   length region via per-entry offset table -- to locate payload.

## Range summary tree

BRIN-style B-tree where each leaf entry is `PgTreRangeLeafEntry`
followed inline by a bloom-filter byte vector of `bloom_bytes`
length.  Internal nodes behave like a standard B-tree keyed on
`range_start_blk`.

## Pending list

A forward-linked chain of pages, head stored in the meta page.
Each page begins with `PgTrePendingHeader` and is followed by a
packed array of `PgTrePendingEntry`.  New entries append at the tail
page; the list is consumed by VACUUM's cleanup phase or the
user-visible `pg_tre_flush()` function.

## Opaque trailer

Every page ends with `PageTreOpaqueData`:

    uint16 page_kind
    uint16 flags
    uint32 format_version       /* per-page on-disk format */

Positioned via `PageGetSpecialPointer(page)`.

`format_version` is the *per-page* on-disk format the page bytes are
in; it can differ from the meta page's index-level `format_version`
while an in-place upgrade is in progress.  All readers accept any
value in `[PG_TRE_FORMAT_VERSION_MIN, PG_TRE_FORMAT_VERSION_LATEST]`.
Writers always emit `LATEST`.

## In-place format upgrade

The machinery for upgrading an existing index to the current on-disk
format without REINDEX:

- Per-page: `PageTreOpaqueData.format_version` records the format the
  page bytes are in.  Writers (page-init, posting-leaf flush, posting-
  leaf rewrite, in-place upgrade) always set it to LATEST.
- Meta: `PgTreMetaPageData.min_page_format_version` is the minimum
  observed across all pages of the index.  When equal to LATEST, the
  index is fully upgraded and future readers can skip per-page
  format dispatch.

SQL surface (1.4.0-dev):

    -- Walk every page; rewrite below LATEST in place.  WAL-logs each
    -- rewritten page as XLOG_PTRE_PAGE_FORMAT_UPGRADE (FORCE_IMAGE).
    -- Holds per-page exclusive lock only briefly.  Updates the meta
    -- page's min_page_format_version after the sweep.
    SELECT pg_tre_upgrade_index('my_idx');

    -- Per-version page counts.  SHARED locks; safe to run concurrently.
    SELECT * FROM pg_tre_index_format_status('my_idx');

    -- O(1) read of meta page's min_page_format_version.
    SELECT pg_tre_index_min_format_version('my_idx');

Readers that decode format-version-dependent layouts dispatch through
`pg_tre_bloom_decode_tuple()` (see `src/util/bloom.c`); the call sites
in `src/am/amscan.c::apply_tuple_bloom_filter` pass the per-page
`format_version` returned out-of-band by
`pg_tre_posting_lookup_tuple_bloom`.  Today v3 and v4 share a decode
path; future format versions plug new arms into
`pg_tre_bloom_decode_tuple` without touching the call sites.

The upgrade is a no-op for v3 -> v4 since the byte layouts are
identical; the framework exists so the next on-disk format change
(planned: variable-width per-tuple blooms in a follow-on commit) can
ship as an in-place rewrite rather than a hard REINDEX requirement.

## On-disk version policy

`format_version` lives in the meta page and is copied to each
page's opaque trailer on write.  Readers accept any value in
`[PG_TRE_FORMAT_VERSION_MIN, PG_TRE_FORMAT_VERSION_LATEST]`; the meta
page's `min_page_format_version` records the minimum across all
pages of the index, updated by `pg_tre_upgrade_index()` after a full
sweep.  Any layout change bumps `PG_TRE_FORMAT_VERSION_LATEST` and
ships with:

- A migration script under `sql/pg_tre--<from>--<to>.sql` exposing
  any new SQL surface.
- For incompatible breaking changes (e.g. v2 -> v3): a unit test
  that reads a snapshot built with the previous version and
  validates field-for-field equality after upgrade.
- For compatible bumps (e.g. v3 -> v4): the in-place upgrade path
  via `pg_tre_upgrade_index()`.

Breaking version bumps (anything that requires REINDEX) are only
permitted at major releases (1.0 -> 2.0); compatible bumps that ship
an in-place upgrade may land in minor releases.
