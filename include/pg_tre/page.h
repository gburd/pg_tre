/*
 * include/pg_tre/page.h - on-disk page layout for the pg_tre index AM.
 *
 * All page structures follow Postgres conventions: the standard 24-byte
 * PageHeaderData at offset 0, an opaque area at the end of the page, and
 * user payload growing between them.  The opaque area identifies the
 * page kind and carries page-kind-specific control data.
 *
 * The exact byte layouts below are load-bearing: they determine on-disk
 * compatibility across pg_tre versions.  Bump PG_TRE_FORMAT_VERSION in
 * pg_tre.h when any change touches a committed format.
 *
 * Page kinds
 * ----------
 *    META    (block 0)          -- superblock
 *    UPPER   (inner nodes)      -- trigram-hash-keyed B-tree interior
 *    UPPER_L (leaf nodes)       -- maps trigram hash -> posting root blk
 *    POSTING (inner nodes)      -- B-tree interior over TID ranges
 *    POSTING_L (leaf)           -- sparsemap blob + payload area
 *    RANGE                      -- BRIN-style range summary leaves
 *    PENDING                    -- fast-update list page
 *
 * All multi-byte fields are native byte order (not portable across archs
 * of differing endianness; this matches every other Postgres index AM).
 */

#ifndef PG_TRE_PAGE_H
#define PG_TRE_PAGE_H

#include "postgres.h"
#include "storage/bufpage.h"
#include "storage/block.h"
#include "storage/itemptr.h"

/* Page-kind tags stored in PageTreOpaqueData.page_kind. */
typedef enum PageTreKind
{
    PG_TRE_PAGE_INVALID   = 0,
    PG_TRE_PAGE_META      = 1,
    PG_TRE_PAGE_UPPER     = 2,
    PG_TRE_PAGE_UPPER_L   = 3,
    PG_TRE_PAGE_POSTING   = 4,
    PG_TRE_PAGE_POSTING_L = 5,
    PG_TRE_PAGE_RANGE     = 6,
    PG_TRE_PAGE_PENDING   = 7,
    PG_TRE_PAGE_RUN_CATALOG = 8     /* format v7: run/level catalog (Phase B1) */
} PageTreKind;

/*
 * Common opaque trailer for every pg_tre page.  Kept small so each
 * page-kind can add kind-specific control words in front of it where
 * needed; we place this struct at the very end of the page's special
 * area and cast appropriately.
 *
 * format_version is the *per-page* format the page bytes are in; it
 * may differ from the meta page's index-level format_version while
 * an in-place upgrade is in progress.  All readers must accept any
 * value in [PG_TRE_FORMAT_VERSION_MIN, PG_TRE_FORMAT_VERSION_LATEST];
 * see include/pg_tre/pg_tre.h.
 */
typedef struct PageTreOpaqueData
{
    uint16      page_kind;          /* PageTreKind */
    uint16      flags;              /* page-kind-specific flags */
    uint32      format_version;     /* per-page on-disk format version */
} PageTreOpaqueData;

typedef PageTreOpaqueData *PageTreOpaque;

#define PageTreGetOpaque(page) \
    ((PageTreOpaque) PageGetSpecialPointer(page))

/* ---- Meta page (always block 0) ---- */

#define PG_TRE_META_MAGIC   0x50545245u /* 'PTRE' */
#define PG_TRE_META_BLKNO   0

typedef struct PgTreMetaPageData
{
    uint32      magic;                  /* PG_TRE_META_MAGIC */
    uint32      format_version;         /* PG_TRE_FORMAT_VERSION */
    uint32      q;                      /* trigram size (default 3) */
    uint32      tri_encoding;           /* 0=byte, 1=codepoint-hash */

    /* Bloom filter parameters */
    uint32      bloom_range_m_bits;     /* per-range bloom size */
    uint16      bloom_range_k;
    uint16      bloom_tuple_m_bits;     /* per-tuple bloom size */
    uint8       bloom_tuple_k;
    uint8       _pad0[3];

    /* Root block numbers */
    BlockNumber root_upper;             /* upper-tree root */
    BlockNumber root_range;             /* range-tree root */

    /* Fast-update pending list */
    BlockNumber pending_head;
    BlockNumber pending_tail;
    uint64      pending_n_entries;

    /* Stats */
    BlockNumber stats_blk;
    uint64      n_trigrams;             /* number of distinct trigrams */
    uint64      n_tuples_indexed;       /* approximate tuple count */
    TransactionId created_xid;

    /* Phase 6: posting cardinality statistics for cost estimation */
    uint64      mean_posting_cardinality;   /* average posting list size */
    uint64      min_posting_cardinality;
    uint64      max_posting_cardinality;
    uint64      stddev_posting_cardinality; /* approximate stddev */

    /*
     * 1.4.0-dev: minimum per-page format_version observed across all
     * pages of this index.  Updated by pg_tre_upgrade_index() once a
     * full sweep has rewritten every page at the target version.
     * Older indexes upgraded in place from a 1.3.x install will have
     * this field zero (pre-1.4.0 reserved[]); pg_tre_meta_read patches
     * a zero value to the index-level format_version on read so the
     * field is always meaningful in memory.
     */
    uint32      min_page_format_version;

    /*
     * 2.0.0 (Phase B1): run/level catalog.  These fields are carved
     * from the former reserved[] space and are zero on a pre-v7
     * (v6) meta page, which pg_tre_meta_read interprets as "one
     * implicit run rooted at root_upper/root_range, no catalog
     * page" -- identical behavior to today.
     *
     *   next_run_id      monotonic 64-bit run-id allocator (never
     *                    wraps, never reuses).  0 on a v6 page.
     *   run_catalog_head first catalog page, or InvalidBlockNumber
     *                    for the single-implicit-run default.
     *   n_runs           count of live runs (0 == implicit single).
     *   max_levels       Hanoi level cap (default 7).
     */
    uint64      next_run_id;
    BlockNumber run_catalog_head;
    uint32      n_runs;
    uint32      max_levels;

    /* Reserved for forward compatibility; zero on new pages */
    uint32      reserved[22];
} PgTreMetaPageData;

typedef PgTreMetaPageData *PgTreMetaPage;

#define PgTreMetaPageGet(page) \
    ((PgTreMetaPage) PageGetContents(page))

/* ---- Posting leaf page ---- */

/*
 * Page-kind-specific flag bits stored in PageTreOpaqueData.flags for
 * POSTING_L pages.
 *
 * PG_TRE_LEAF_DELETED marks a posting leaf that VACUUM has unlinked from
 * its right-link chain (it is now empty and no longer reachable as a
 * chain member from its left sibling).  A deleted leaf is NOT yet free:
 * a concurrent scan that copied a stale right_link before the unlink may
 * still land on it, so the page must remain a coherent waypoint.  Its
 * right_link is preserved (a stale scanner continues correctly to the
 * real successor) and its sparsemap is empty (matches no TIDs).  The
 * deleted-leaf control block (PgTrePostingDeletedHeader) overlays the
 * content area and carries the full XID at which the page was deleted;
 * the page is only handed to the FSM by a later VACUUM once that XID is
 * old enough that no snapshot could still be traversing the pre-unlink
 * chain (PG_TRE_LEAF_DELETED -> recycled, nbtree's safexid discipline).
 */
#define PG_TRE_LEAF_DELETED   0x0001

typedef struct PgTrePostingLeafHeader
{
    BlockNumber right_link;             /* Lehman-Yao right sibling */
    uint64      min_tid;                /* packed (blk<<16 | offset) */
    uint64      max_tid;
    uint32      sparsemap_bytes;        /* size of the sparsemap blob */
    uint32      payload_bytes;          /* size of the payload region */
    uint32      payload_offset;         /* offset from page start */
    uint16      n_entries;              /* sm_cardinality cache */
    uint16      _pad;
} PgTrePostingLeafHeader;

/*
 * Control block for a DELETED (unlinked, awaiting recycle) posting leaf.
 * It reuses the leaf header's right_link slot (kept valid for stale
 * scanners) and stamps the deletion XID.  Stored at PageGetContents();
 * the leaf header's leading right_link field aliases this struct's
 * right_link so a stale scanner reading the page as a normal leaf still
 * follows a correct (empty sparsemap, valid right_link) waypoint.
 */
typedef struct PgTrePostingDeletedHeader
{
    BlockNumber right_link;             /* MUST alias PgTrePostingLeafHeader.right_link */
    uint32      _pad0;
    uint64      deleted_xid_value;      /* FullTransactionId.value at unlink time */
} PgTrePostingDeletedHeader;

/*
 * Layout of a posting leaf page:
 *
 *   [ PageHeader ]                   24 B
 *   [ PgTrePostingLeafHeader ]       40 B
 *   [ sparsemap blob ]               sparsemap_bytes
 *   [ ... free space ...           ]
 *   [ payload region (grows down) ]  payload_bytes
 *   [ PageTreOpaqueData ]             8 B
 */

/* ---- Upper-tree leaf entry ---- */

typedef struct PgTreUpperLeafEntry
{
    uint64      trigram_hash;
    BlockNumber posting_root;           /* posting-tree root, or InvalidBlockNumber for inline */
    uint32      inline_bytes;           /* nonzero if posting is inlined after this entry */
} PgTreUpperLeafEntry;

/* ---- Range summary leaf entry ---- */

typedef struct PgTreRangeLeafEntry
{
    BlockNumber range_start_blk;
    BlockNumber range_end_blk;          /* exclusive */
    uint32      bloom_bytes;            /* size of bloom that follows */
    /* bloom bytes follow inline */
} PgTreRangeLeafEntry;

/* ---- Run/level catalog (format v7, Phase B1) ---- */

/* Per-run flags in PgTreRun.flags. */
#define PG_TRE_RUN_LIVE      0x0001   /* run participates in scans */
#define PG_TRE_RUN_TOMBSTONE 0x0002   /* run holds only tombstones (deletes) */

/*
 * One run: a self-contained pg_tre sub-index (upper-tree + range
 * tier + posting trees) over a slice of ingested rows.  run_id is
 * monotonic 64-bit (never wraps, never reused), mirroring aether's
 * RunId; it is NOT a transaction id (B1 has no visibility axis).
 * [min_trigram_hash, max_trigram_hash] is the run-skip range
 * filter (aether's Surf analogue): a scan whose queried trigram
 * hash is outside the range skips this run.
 */
typedef struct PgTreRun
{
    uint64      run_id;
    uint32      level;              /* Hanoi level, 1-based (0 = nursery) */
    uint32      flags;
    BlockNumber root_upper;
    BlockNumber root_range;
    uint32      _pad0;
    uint64      n_tuples;
    uint64      n_trigrams;
    uint64      min_trigram_hash;
    uint64      max_trigram_hash;
} PgTreRun;                         /* 64 bytes, 8-aligned */

/*
 * Run catalog page header: an array of PgTreRun follows immediately
 * after PageGetContents.  Pages chain via right_link when there are
 * more runs than fit on one page (~126 runs/page).
 */
typedef struct PgTreRunCatalogHeader
{
    BlockNumber right_link;         /* next catalog page, or Invalid */
    uint32      n_entries;          /* PgTreRun records on this page */
    uint32      _pad0;
} PgTreRunCatalogHeader;

/* ---- Range summary page header (format v5+) ----
 *
 * Range pages at format_version >= 5 begin their content area with this
 * 8-byte header; range entries follow immediately after.  Pages chain
 * via right_link to support more entries than fit on one page.
 *
 * Layout of a v5 range page:
 *
 *   [ PageHeader              ]   24 B
 *   [ PgTreRangeHeader        ]    8 B   (right_link, n_entries, _pad)
 *   [ PgTreRangeLeafEntry  #0 ]   variable (entry + inline bloom bytes)
 *   [ PgTreRangeLeafEntry  #1 ]
 *   ...
 *   [ ... free space ...     ]
 *   [ PageTreOpaqueData       ]    8 B
 *
 * meta.root_range points to the FIRST page of the chain.  right_link is
 * InvalidBlockNumber on the last page.  pd_lower marks the end of the
 * last entry written on the page (inclusive of the header).
 *
 * Format v3 / v4 range pages do NOT have this header; entries start at
 * PageGetContents() directly.  Readers dispatch on opq->format_version.
 */
typedef struct PgTreRangeHeader
{
    BlockNumber right_link;     /* next range page, or InvalidBlockNumber */
    uint16      n_entries;      /* entries written on this page */
    uint16      _pad;           /* zero */
} PgTreRangeHeader;

/* ---- Pending list page ---- */

typedef struct PgTrePendingHeader
{
    BlockNumber next_page;
    uint32      n_entries;
    uint32      used_bytes;
} PgTrePendingHeader;

typedef struct PgTrePendingEntry
{
    uint64      trigram_hash;
    uint64      tid;                    /* packed (blk<<16 | offset) */
    uint32      position;               /* byte offset in original value */
    uint32      _pad;
} PgTrePendingEntry;

/* ---- TID packing ---- */

static inline uint64
pg_tre_pack_tid(ItemPointer tid)
{
    uint64 blk = BlockIdGetBlockNumber(&tid->ip_blkid);
    uint64 off = tid->ip_posid;
    return (blk << 16) | off;
}

static inline void
pg_tre_unpack_tid(uint64 packed, ItemPointer out)
{
    BlockNumber blk = (BlockNumber) (packed >> 16);
    OffsetNumber off = (OffsetNumber) (packed & 0xFFFF);
    ItemPointerSet(out, blk, off);
}

#endif /* PG_TRE_PAGE_H */
