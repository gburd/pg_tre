/*
 * src/pages/upper.c - upper tree (trigram-hash -> posting root).
 *
 * Phase 1: B-tree page format + Lehman-Yao descent.
 * Phase 2: bulk loader.
 * Phase 4: single-entry inserts from pending-list merge.
 *
 * ---- Phase 1/2 implementation ----
 *
 * Upper tree is a B-tree keyed by uint64 trigram_hash.  Leaf pages
 * store PgTreUpperLeafEntry[] sorted by hash; each entry may inline
 * a small sparsemap or point to a posting-tree root.
 *
 * Internal pages store (first_key, child_blk) pairs.
 *
 * Lehman-Yao right-links allow concurrent readers during splits (Phase 3).
 * For Phase 2, we only need bulk-load, which builds the tree bottom-up
 * from a sorted list of (hash, root|inline) pairs.
 */

#include "postgres.h"

#include "access/xlog.h"
#include "access/xloginsert.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lockdefs.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "pg_tre/buffer.h"
#include "pg_tre/meta.h"
#include "pg_tre/page.h"
#include "pg_tre/posting.h"
#include "pg_tre/upper.h"
#include "pg_tre/xlog.h"

/* Upper-tree internal page entry: (first_key, child_blk). */
typedef struct PgTreUpperInternalEntry
{
    uint64      first_key;
    BlockNumber child_blk;
} PgTreUpperInternalEntry;

/* Maximum entries per page (conservative estimate). */
#define UPPER_LEAF_MAX_ENTRIES \
    ((BLCKSZ - sizeof(PageHeaderData) - sizeof(PageTreOpaqueData) - 64) / \
     sizeof(PgTreUpperLeafEntry))

#define UPPER_INTERNAL_MAX_ENTRIES \
    ((BLCKSZ - sizeof(PageHeaderData) - sizeof(PageTreOpaqueData) - 64) / \
     sizeof(PgTreUpperInternalEntry))

/*
 * Bulk-load builder state.  Accumulates leaf pages, then builds
 * internal levels bottom-up.
 */
typedef struct UpperBulkState
{
    Relation    index;
    MemoryContext mcxt;

    /* Current leaf being built. */
    PgTreUpperLeafEntry *leaf_entries;
    uint8       *leaf_inline_data;
    int         leaf_n_entries;
    Size        leaf_inline_used;
    Size        leaf_inline_alloced;

    /* Completed leaf blocks. */
    BlockNumber *leaf_blocks;
    uint64      *leaf_first_keys;
    int         n_leaves;
    int         leaves_alloced;
} UpperBulkState;

/* ---- Phase 2: Bulk loader ---- */

/*
 * Iterator function type for bulk-load.  Callback returns true and
 * fills *hash, *root, *inline_data, *inline_bytes for the next entry,
 * or returns false on EOF.  Entries must be sorted by hash ascending.
 */
typedef bool (*upper_bulkload_iter_func)(void *ctx, uint64 *hash,
                                         BlockNumber *root,
                                         const uint8 **inline_data,
                                         Size *inline_bytes);

static UpperBulkState *
upper_bulkload_init(Relation index)
{
    UpperBulkState *state;
    MemoryContext oldcxt;

    state = (UpperBulkState *) palloc0(sizeof(UpperBulkState));
    state->mcxt = AllocSetContextCreate(CurrentMemoryContext,
                                        "upper bulkload context",
                                        ALLOCSET_DEFAULT_SIZES);
    oldcxt = MemoryContextSwitchTo(state->mcxt);

    state->index = index;
    state->leaf_entries = (PgTreUpperLeafEntry *)
        palloc(UPPER_LEAF_MAX_ENTRIES * sizeof(PgTreUpperLeafEntry));
    state->leaf_inline_alloced = 8192;
    state->leaf_inline_data = (uint8 *) palloc(state->leaf_inline_alloced);
    state->leaf_n_entries = 0;
    state->leaf_inline_used = 0;

    state->leaves_alloced = 64;
    state->leaf_blocks = (BlockNumber *)
        palloc(state->leaves_alloced * sizeof(BlockNumber));
    state->leaf_first_keys = (uint64 *)
        palloc(state->leaves_alloced * sizeof(uint64));
    state->n_leaves = 0;

    MemoryContextSwitchTo(oldcxt);
    return state;
}

/*
 * Flush the current leaf page to disk.
 */
static void
upper_flush_leaf(UpperBulkState *state)
{
    Buffer  buf;
    Page    page;
    char   *dest;
    Size    entries_size;

    if (state->leaf_n_entries == 0)
        return;

    /* Extend and initialize a new leaf page. */
    buf = pg_tre_extend(state->index, PG_TRE_PAGE_UPPER_L);
    page = BufferGetPage(buf);

    /*
     * Layout: PageHeader | PgTreUpperLeafEntry[] | inline blobs | Opaque
     *
     * Store the entry array first, then pack inline blobs after.
     */
    dest = (char *) PageGetContents(page);
    entries_size = state->leaf_n_entries * sizeof(PgTreUpperLeafEntry);
    memcpy(dest, state->leaf_entries, entries_size);
    dest += entries_size;

    /* Copy inline blobs and adjust entry offsets. */
    if (state->leaf_inline_used > 0)
    {
        memcpy(dest, state->leaf_inline_data, state->leaf_inline_used);

        /*
         * Phase 2 TODO: fix up inline_data pointers in entries.
         * For now, the upper-tree lookup code handles inline data
         * by pointing past the entry array.  A proper implementation
         * would store page-relative offsets in the entries.
         */
    }

    /* Update pd_lower. */
    ((PageHeader) page)->pd_lower = dest + state->leaf_inline_used - (char *) page;

    /* Record n_entries in the page's opaque flags so readers don't have
     * to back-compute it from pd_lower (which includes variable-length
     * inline blobs after the entry array). */
    PageTreGetOpaque(page)->flags = (uint16) state->leaf_n_entries;

    /* WAL-log as full-page image. */
    if (RelationNeedsWAL(state->index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT | REGBUF_STANDARD);

        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_UPPER_INSERT);
        PageSetLSN(page, recptr);
    }

    MarkBufferDirty(buf);

    /* Record the leaf block and its first key. */
    if (state->n_leaves >= state->leaves_alloced)
    {
        state->leaves_alloced *= 2;
        state->leaf_blocks = (BlockNumber *)
            repalloc(state->leaf_blocks,
                     state->leaves_alloced * sizeof(BlockNumber));
        state->leaf_first_keys = (uint64 *)
            repalloc(state->leaf_first_keys,
                     state->leaves_alloced * sizeof(uint64));
    }
    state->leaf_blocks[state->n_leaves] = BufferGetBlockNumber(buf);
    state->leaf_first_keys[state->n_leaves] = state->leaf_entries[0].trigram_hash;
    state->n_leaves++;

    UnlockReleaseBuffer(buf);

    /* Reset current leaf. */
    state->leaf_n_entries = 0;
    state->leaf_inline_used = 0;
}

/*
 * Add one entry to the current leaf page.  Flushes if the page is full.
 */
static void
upper_add_leaf_entry(UpperBulkState *state, uint64 hash, BlockNumber root,
                     const uint8 *inline_data, Size inline_bytes)
{
    PgTreUpperLeafEntry *ent;
    Size total_used;
    Size page_budget;

    /*
     * Page budget: full page minus header and opaque trailer, leaving a
     * small safety margin.  Writing past this boundary clobbers the
     * opaque trailer (format_version/page_kind) of the page.
     */
    page_budget = BLCKSZ
                - MAXALIGN(SizeOfPageHeaderData)
                - MAXALIGN(sizeof(PageTreOpaqueData))
                - 64;   /* safety margin */

    total_used = (state->leaf_n_entries + 1) * sizeof(PgTreUpperLeafEntry)
               + state->leaf_inline_used + inline_bytes;

    /* Check if we need to flush the current leaf. */
    if (state->leaf_n_entries >= UPPER_LEAF_MAX_ENTRIES ||
        total_used > page_budget)
    {
        upper_flush_leaf(state);
    }

    /* Append the entry. */
    ent = &state->leaf_entries[state->leaf_n_entries++];
    ent->trigram_hash = hash;
    ent->posting_root = root;
    ent->inline_bytes = (uint32) inline_bytes;

    if (inline_bytes > 0)
    {
        /* Copy inline blob to the inline data buffer. */
        if (state->leaf_inline_used + inline_bytes > state->leaf_inline_alloced)
        {
            state->leaf_inline_alloced *= 2;
            state->leaf_inline_data = (uint8 *)
                repalloc(state->leaf_inline_data, state->leaf_inline_alloced);
        }
        memcpy(state->leaf_inline_data + state->leaf_inline_used,
               inline_data, inline_bytes);
        /*
         * Store the offset in a temporary way; we'll fix it during flush.
         * For simplicity in Phase 2, just store as offset from inline_data
         * base.
         */
        state->leaf_inline_used += inline_bytes;
    }
}

/*
 * Build internal level from an array of (first_key, child_blk) pairs.
 * Returns the root block number of the new level.
 */
static BlockNumber
upper_build_internal_level(Relation index, uint64 *keys, BlockNumber *blocks,
                           int n_entries)
{
    Buffer  buf;
    Page    page;
    PgTreUpperInternalEntry *entries;
    int     i;
    int     entries_per_page;
    BlockNumber root_blk = InvalidBlockNumber;

    if (n_entries == 0)
        return InvalidBlockNumber;

    if (n_entries == 1)
    {
        /* Single child: that child is the root. */
        return blocks[0];
    }

    /*
     * Multiple children: build one or more internal pages.  For Phase 2,
     * we assume n_entries fits in one page.  If not, we'd need multiple
     * internal pages and recursion.
     */
    entries_per_page = UPPER_INTERNAL_MAX_ENTRIES;
    if (n_entries > entries_per_page)
        elog(ERROR, "pg_tre: upper-tree internal level overflow "
             "(%d entries > %d max); Phase 2 does not support multi-level internals",
             n_entries, entries_per_page);

    /* Allocate a single internal page. */
    buf = pg_tre_extend(index, PG_TRE_PAGE_UPPER);
    page = BufferGetPage(buf);

    entries = (PgTreUpperInternalEntry *) PageGetContents(page);
    for (i = 0; i < n_entries; i++)
    {
        entries[i].first_key = keys[i];
        entries[i].child_blk = blocks[i];
    }

    ((PageHeader) page)->pd_lower =
        (char *) &entries[n_entries] - (char *) page;

    /* WAL-log. */
    if (RelationNeedsWAL(index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT | REGBUF_STANDARD);

        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_UPPER_INSERT);
        PageSetLSN(page, recptr);
    }

    MarkBufferDirty(buf);
    root_blk = BufferGetBlockNumber(buf);
    UnlockReleaseBuffer(buf);

    return root_blk;
}

/*
 * Bulk-load the upper tree from a sorted iterator.
 */
BlockNumber
pg_tre_upper_bulkload(Relation index, upper_bulkload_iter_func iter,
                      void *iter_ctx)
{
    UpperBulkState *state;
    uint64      hash;
    BlockNumber root;
    const uint8 *inline_data;
    Size        inline_bytes;
    BlockNumber tree_root;

    state = upper_bulkload_init(index);

    /* Phase 1: build all leaf pages. */
    while (iter(iter_ctx, &hash, &root, &inline_data, &inline_bytes))
    {
        
        upper_add_leaf_entry(state, hash, root, inline_data, inline_bytes);
    }
    upper_flush_leaf(state);

    if (state->n_leaves == 0)
    {
        /* Empty index. */
        MemoryContextDelete(state->mcxt);
        return InvalidBlockNumber;
    }

    /* Phase 2: build internal levels bottom-up. */
    tree_root = upper_build_internal_level(index, state->leaf_first_keys,
                                           state->leaf_blocks,
                                           state->n_leaves);

    MemoryContextDelete(state->mcxt);
    return tree_root;
}

/* ---- Phase 1/4: Single-entry insert (sketch) ---- */

void
pg_tre_upper_insert(Relation index, uint64 hash, BlockNumber root,
                    const uint8 *inline_data, Size inline_bytes)
{
    /*
     * Phase 4: descend the tree to find the correct leaf, insert the
     * entry, split if needed.  For Phase 2, this is not yet used.
     */
    elog(ERROR, "pg_tre: upper-tree insert not yet implemented (Phase 4)");
}

/* ---- Upper-tree lookup (Phase 2 stub, Phase 3 real) ---- */

bool
pg_tre_upper_lookup(Relation index, uint64 trigram_hash, PgTreUpperRef *out)
{
    PgTreMetaPageData meta;
    Buffer  buf;
    Page    page;
    PageTreOpaque opq;
    int     i;

    /* Read meta to get root_upper. */
    pg_tre_meta_read(index, &meta);
    if (meta.root_upper == InvalidBlockNumber)
    {
        /* Empty index. */
        out->upper_buf = InvalidBuffer;
        out->root = InvalidBlockNumber;
        out->inline_data = NULL;
        out->inline_bytes = 0;
        return false;
    }

    /*
     * Descend to find the leaf.  For Phase 2 with a single internal
     * page or a single leaf, this is straightforward.
     */
    buf = pg_tre_read(index, meta.root_upper, PG_TRE_PAGE_INVALID,
                      BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);
    opq = PageTreGetOpaque(page);

    if (opq->page_kind == PG_TRE_PAGE_UPPER_L)
    {
        /* Root is a leaf; search it. */
        PgTreUpperLeafEntry *entries;
        int n_entries;

        entries = (PgTreUpperLeafEntry *) PageGetContents(page);
        n_entries = PageTreGetOpaque(page)->flags;
        if (n_entries == 0)
        {
            /* Back-compat fallback: pages written before the flags
             * counter was added may have flags=0 even when entries
             * exist.  Compute conservatively using sizeof(entry);
             * this over-counts when inline blobs are present but
             * only affects older indexes -- they can be rebuilt
             * with REINDEX. */
            n_entries = (((PageHeader) page)->pd_lower
                         - sizeof(PageHeaderData))
                        / sizeof(PgTreUpperLeafEntry);
        }

        /* Linear search for trigram_hash.  Since inline blobs are
         * concatenated after the entry array without per-entry offsets,
         * we compute the matched entry's blob offset by summing
         * inline_bytes of all preceding entries.  Entries must be
         * sorted by hash during bulkload (they are). */
        {
            Size inline_offset = 0;
            for (i = 0; i < n_entries; i++)
            {
                if (entries[i].trigram_hash == trigram_hash)
                {
                    out->upper_buf = buf;
                    out->root = entries[i].posting_root;
                    if (entries[i].inline_bytes > 0)
                    {
                        out->inline_data = ((const uint8 *) &entries[n_entries])
                                         + inline_offset;
                        out->inline_bytes = entries[i].inline_bytes;
                    }
                    else
                    {
                        out->inline_data = NULL;
                        out->inline_bytes = 0;
                    }
                    return true;
                }
                inline_offset += entries[i].inline_bytes;
            }
        }

        /* Not found. */
        UnlockReleaseBuffer(buf);
        out->upper_buf = InvalidBuffer;
        return false;
    }
    else if (opq->page_kind == PG_TRE_PAGE_UPPER)
    {
        /*
         * Internal page: descend to the correct child.  For Phase 2,
         * we only have one internal page.
         */
        PgTreUpperInternalEntry *entries;
        int n_entries;
        BlockNumber child_blk = InvalidBlockNumber;

        entries = (PgTreUpperInternalEntry *) PageGetContents(page);
        n_entries = (((PageHeader) page)->pd_lower - sizeof(PageHeaderData)) /
                    sizeof(PgTreUpperInternalEntry);

        /* Find the rightmost entry whose first_key <= trigram_hash. */
        for (i = 0; i < n_entries; i++)
        {
            if (entries[i].first_key <= trigram_hash)
                child_blk = entries[i].child_blk;
            else
                break;
        }

        UnlockReleaseBuffer(buf);

        if (child_blk == InvalidBlockNumber)
        {
            out->upper_buf = InvalidBuffer;
            return false;
        }

        /* Read the child leaf and search it. */
        buf = pg_tre_read(index, child_blk, PG_TRE_PAGE_UPPER_L,
                          BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);

        PgTreUpperLeafEntry *entries2;
        int n_entries2;

        entries2 = (PgTreUpperLeafEntry *) PageGetContents(page);
        n_entries2 = (((PageHeader) page)->pd_lower - sizeof(PageHeaderData)) /
                     sizeof(PgTreUpperLeafEntry);

        for (i = 0; i < n_entries2; i++)
        {
            if (entries2[i].trigram_hash == trigram_hash)
            {
                out->upper_buf = buf;
                out->root = entries2[i].posting_root;
                if (entries2[i].inline_bytes > 0)
                {
                    out->inline_data = (const uint8 *) &entries2[n_entries2];
                    out->inline_bytes = entries2[i].inline_bytes;
                }
                else
                {
                    out->inline_data = NULL;
                    out->inline_bytes = 0;
                }
                return true;
            }
        }

        UnlockReleaseBuffer(buf);
        out->upper_buf = InvalidBuffer;
        return false;
    }
    else
    {
        UnlockReleaseBuffer(buf);
        elog(ERROR, "pg_tre: unexpected page kind %u at root_upper",
             opq->page_kind);
    }
}

void
pg_tre_upper_release(PgTreUpperRef *ref)
{
    if (BufferIsValid(ref->upper_buf))
        UnlockReleaseBuffer(ref->upper_buf);
}
