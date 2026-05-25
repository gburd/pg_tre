# pg_tre on-disk page format (v4)

Authoritative declarations live in `include/pg_tre/page.h`.  This
document is the narrative reference and the place where format
changes are reviewed before they are locked in.

All multi-byte fields use native byte order.  All pages are the
Postgres cluster page size (default 8 KiB) and begin with the
standard `PageHeaderData` at offset 0 and end with a
`PageTreOpaqueData` trailer in the special area.

## Format version history

- **v4 (1.4 / variable-width blooms)**: Per-tuple bloom width is
  selected per row from the row's distinct-trigram count.  Each
  payload entry now carries a (width_code, k) byte pair before the
  bloom bit array; readers map width_code to bit count via the
  table in `include/pg_tre/bloom.h`.  See
  `doc/specs/variable-width-blooms.md`.
- **v3 (Phase 4.2)**: Multi-leaf posting trees. When a single trigram's
  posting exceeds ~8 KB, the builder splits it across multiple leaves
  linked by `right_link` (Lehman-Yao convention). Each leaf stores
  `min_tid` and `max_tid` bounds. Readers traverse the chain to find
  the leaf containing a target TID.
- **v2 (Phase 3.5)**: Codepoint-based trigrams.
- **v1**: Initial format with byte-based trigrams.

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
    |   per-TID:                        |
    |     uint16 n_positions            |
    |     uint32 positions[n_positions] |
    |     uint8  bloom_width_code       |  v4: maps to bits via
    |     uint8  bloom_k                |       pg_tre_bloom_width_from_code
    |     uint8  bloom_bits[width/8]    |       (omitted when width_code=255)
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
    uint32 format_version

Positioned via `PageGetSpecialPointer(page)`.

## On-disk version policy

`format_version` lives in the meta page and is copied to each
page's opaque trailer on write.  Readers assert the two match.  Any
layout change bumps the version and ships with:

- A dump/restore-compatible script under `sql/pg_tre--<from>--<to>.sql`.
- A unit test that reads a snapshot built with the previous version
  and validates field-for-field equality after upgrade.

Version bumps are only permitted at major releases (1.0 -> 2.0).
