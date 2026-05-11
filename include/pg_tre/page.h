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
    PG_TRE_PAGE_PENDING   = 7
} PageTreKind;

/*
 * Common opaque trailer for every pg_tre page.  Kept small so each
 * page-kind can add kind-specific control words in front of it where
 * needed; we place this struct at the very end of the page's special
 * area and cast appropriately.
 */
typedef struct PageTreOpaqueData
{
    uint16      page_kind;          /* PageTreKind */
    uint16      flags;              /* page-kind-specific flags */
    uint32      format_version;     /* copy of meta.format_version */
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

    /* Reserved for forward compatibility; zero on new pages */
    uint32      reserved[28];
} PgTreMetaPageData;

typedef PgTreMetaPageData *PgTreMetaPage;

#define PgTreMetaPageGet(page) \
    ((PgTreMetaPage) PageGetContents(page))

/* ---- Posting leaf page ---- */

typedef struct PgTrePostingLeafHeader
{
    BlockNumber right_link;             /* Lehman-Yao right sibling */
    uint64      min_tid;                /* packed (blk<<16 | offset) */
    uint64      max_tid;
    uint32      sparsemap_bytes;        /* size of the sparsemap blob */
    uint32      payload_bytes;          /* size of the payload region */
    uint32      payload_offset;         /* offset from page start */
    uint16      n_entries;              /* sparsemap_cardinality cache */
    uint16      _pad;
} PgTrePostingLeafHeader;

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
