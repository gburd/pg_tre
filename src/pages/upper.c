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
#include "pg_tre/coalesced.h"
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
         * Phase 8 perf TODO: fix up inline_data pointers in entries.
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

    MarkBufferDirty(buf);

    /* WAL-log as full-page image.  MarkBufferDirty must precede
     * XLogRegisterBuffer (PG18 asserts buffer is dirty + exclusively
     * locked). */
    if (RelationNeedsWAL(state->index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);

        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_UPPER_INSERT);
        PageSetLSN(page, recptr);
    }

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
 *
 * inline_bytes encodes the storage class (see PgTreUpperLeafEntry in
 * page.h).  For a coalesced entry the PG_TRE_COALESCED_FLAG bit is set
 * and the value is a (flag | slot) marker, NOT a blob length: it is
 * stored verbatim in the entry but contributes no inline bytes to the
 * leaf, and inline_data is NULL.
 */
static void
upper_add_leaf_entry(UpperBulkState *state, uint64 hash, BlockNumber root,
                     const uint8 *inline_data, Size inline_bytes)
{
    PgTreUpperLeafEntry *ent;
    Size total_used;
    Size page_budget;
    bool is_coalesced = (((uint32) inline_bytes) & PG_TRE_COALESCED_FLAG) != 0;
    Size blob_bytes = is_coalesced ? 0 : inline_bytes;

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
               + state->leaf_inline_used + blob_bytes;

    /* Check if we need to flush the current leaf. */
    if (state->leaf_n_entries >= UPPER_LEAF_MAX_ENTRIES ||
        total_used > page_budget)
    {
        upper_flush_leaf(state);
    }

    /* Append the entry.  inline_bytes is stored verbatim so the
     * coalesced (flag | slot) marker round-trips into the entry. */
    ent = &state->leaf_entries[state->leaf_n_entries++];
    ent->trigram_hash = hash;
    ent->posting_root = root;
    ent->inline_bytes = (uint32) inline_bytes;

    if (blob_bytes > 0)
    {
        /* Copy inline blob to the inline data buffer. */
        if (state->leaf_inline_used + blob_bytes > state->leaf_inline_alloced)
        {
            state->leaf_inline_alloced *= 2;
            state->leaf_inline_data = (uint8 *)
                repalloc(state->leaf_inline_data, state->leaf_inline_alloced);
        }
        memcpy(state->leaf_inline_data + state->leaf_inline_used,
               inline_data, blob_bytes);
        /*
         * Store the offset in a temporary way; we'll fix it during flush.
         * For simplicity in Phase 2, just store as offset from inline_data
         * base.
         */
        state->leaf_inline_used += blob_bytes;
    }
}

/*
 * Build internal levels from an array of (first_key, child_blk)
 * pairs.  Returns the root block number of the new tree.
 *
 * Recursive: when the input array is too large to fit on one
 * internal page, splits into ceil(n / entries_per_page) sibling
 * pages and recurses on the first-keys of those siblings.
 * Continues until one root remains.
 *
 * Memory: allocates a temporary array for the next level's keys
 * and blocks at each recursion step.  Sized to ceil(n / fanout)
 * which shrinks geometrically; total auxiliary memory is O(n).
 */
static BlockNumber
upper_build_internal_level(Relation index, uint64 *keys, BlockNumber *blocks,
                           int n_entries)
{
    int     entries_per_page;
    int     n_pages;
    int     i, page_idx;
    uint64 *next_keys;
    BlockNumber *next_blocks;
    BlockNumber root_blk;

    if (n_entries == 0)
        return InvalidBlockNumber;

    if (n_entries == 1)
    {
        /* Single child: that child is the root. */
        return blocks[0];
    }

    entries_per_page = UPPER_INTERNAL_MAX_ENTRIES;
    n_pages = (n_entries + entries_per_page - 1) / entries_per_page;

    /*
     * Build n_pages sibling internal pages at this level.
     * Each page covers a contiguous run of [start, end) input
     * entries.  Distribute as evenly as possible to avoid a
     * tail page with very few entries; not strictly necessary
     * for correctness but reduces tree depth on edge cases.
     */
    next_keys   = (uint64 *) palloc(n_pages * sizeof(uint64));
    next_blocks = (BlockNumber *) palloc(n_pages * sizeof(BlockNumber));

    for (page_idx = 0; page_idx < n_pages; page_idx++)
    {
        Buffer  buf;
        Page    page;
        PgTreUpperInternalEntry *entries;
        int     start = (int) ((int64) page_idx * n_entries / n_pages);
        int     end   = (int) ((int64) (page_idx + 1) * n_entries / n_pages);
        int     this_n = end - start;

        Assert(this_n > 0);
        Assert(this_n <= entries_per_page);

        buf = pg_tre_extend(index, PG_TRE_PAGE_UPPER);
        page = BufferGetPage(buf);
        entries = (PgTreUpperInternalEntry *) PageGetContents(page);
        for (i = 0; i < this_n; i++)
        {
            entries[i].first_key = keys[start + i];
            entries[i].child_blk = blocks[start + i];
        }

        ((PageHeader) page)->pd_lower =
            (char *) &entries[this_n] - (char *) page;

        MarkBufferDirty(buf);

        /* MarkBufferDirty must precede XLogRegisterBuffer (PG18
         * asserts buffer is dirty + exclusively locked). */
        if (RelationNeedsWAL(index))
        {
            XLogRecPtr recptr;

            XLogBeginInsert();
            XLogRegisterBuffer(0, buf,
                               REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
            recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_UPPER_INSERT);
            PageSetLSN(page, recptr);
        }

        next_keys[page_idx]   = keys[start];   /* first key on this page */
        next_blocks[page_idx] = BufferGetBlockNumber(buf);
        UnlockReleaseBuffer(buf);
    }

    /* Recurse on the parent level. */
    root_blk = upper_build_internal_level(index, next_keys, next_blocks,
                                          n_pages);

    pfree(next_keys);
    pfree(next_blocks);
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

    /* Resolve against the single index root (the implicit run). */
    pg_tre_meta_read(index, &meta);
    return pg_tre_upper_lookup_root(index, meta.root_upper, trigram_hash, out);
}

/*
 * Root-parameterized lookup (Phase B1.2): resolve a trigram against
 * an arbitrary upper-tree root, so the multi-run scan can resolve a
 * trigram independently within each run.  Identical descent logic to
 * pg_tre_upper_lookup; the only difference is the caller supplies the
 * root rather than reading meta.root_upper.
 */
bool
pg_tre_upper_lookup_root(Relation index, BlockNumber root_upper,
                         uint64 trigram_hash, PgTreUpperRef *out)
{
    Buffer  buf;
    Page    page;
    PageTreOpaque opq;
    BlockNumber blk;
    int     i;

    if (root_upper == InvalidBlockNumber)
    {
        /* Empty run/index. */
        out->upper_buf = InvalidBuffer;
        out->root = InvalidBlockNumber;
        out->inline_data = NULL;
        out->inline_bytes = 0;
        out->owns_inline = false;
        return false;
    }

    /*
     * Descend through internal levels until we reach a leaf page.
     * The tree may have one or more internal levels (multi-level
     * support landed in 1.4.x to handle large vocabularies); each
     * level dispatches to a single child by selecting the rightmost
     * entry whose first_key <= trigram_hash.
     */
    blk = root_upper;
    for (;;)
    {
        BlockNumber child_blk = InvalidBlockNumber;
        PgTreUpperInternalEntry *internal_entries;
        int n_internal;

        buf = pg_tre_read(index, blk, PG_TRE_PAGE_INVALID,
                          BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);
        opq = PageTreGetOpaque(page);

        if (opq->page_kind == PG_TRE_PAGE_UPPER_L)
            break;          /* Reached a leaf; search below. */

        if (opq->page_kind != PG_TRE_PAGE_UPPER)
        {
            UnlockReleaseBuffer(buf);
            elog(ERROR,
                 "pg_tre: unexpected page kind %u during upper-tree descent",
                 opq->page_kind);
        }

        /* Internal page: pick child by first_key <= trigram_hash. */
        internal_entries = (PgTreUpperInternalEntry *) PageGetContents(page);
        n_internal = (((PageHeader) page)->pd_lower - sizeof(PageHeaderData)) /
                     sizeof(PgTreUpperInternalEntry);
        for (i = 0; i < n_internal; i++)
        {
            if (internal_entries[i].first_key <= trigram_hash)
                child_blk = internal_entries[i].child_blk;
            else
                break;
        }
        UnlockReleaseBuffer(buf);

        if (child_blk == InvalidBlockNumber)
        {
            out->upper_buf = InvalidBuffer;
            out->owns_inline = false;
            return false;
        }
        blk = child_blk;
    }

    /* `buf` / `page` now hold a leaf page. */
    {
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
                uint32 ib = entries[i].inline_bytes;

                if (entries[i].trigram_hash == trigram_hash)
                {
                    out->upper_buf = buf;
                    out->root = entries[i].posting_root;
                    out->owns_inline = false;

                    if (ib & PG_TRE_COALESCED_FLAG)
                    {
                        /*
                         * Coalesced posting (format v8): posting_root
                         * is a PG_TRE_PAGE_POSTING_COALESCED block and
                         * the low bits are the slot index.  Resolve the
                         * slot into a palloc'd copy and present it as an
                         * ordinary inline blob so every downstream
                         * consumer (materialize / scan / cardinality /
                         * lookup) is unchanged.  We can release the
                         * upper-tree leaf buffer immediately since the
                         * resolved bytes are an owned copy.
                         */
                        uint16 slot_idx = (uint16)
                            (ib & PG_TRE_COALESCED_SLOT_MASK);
                        BlockNumber cblk = entries[i].posting_root;
                        Size        sm_len = 0;
                        uint8      *blob;

                        UnlockReleaseBuffer(buf);
                        out->upper_buf = InvalidBuffer;

                        blob = pg_tre_coalesced_resolve_slot(index, cblk,
                                                             slot_idx,
                                                             trigram_hash,
                                                             &sm_len);
                        out->root = InvalidBlockNumber;
                        out->inline_data = blob;     /* NULL if tombstoned */
                        out->inline_bytes = sm_len;
                        out->owns_inline = (blob != NULL);
                        return true;
                    }

                    if (ib > 0)
                    {
                        out->inline_data = ((const uint8 *) &entries[n_entries])
                                         + inline_offset;
                        out->inline_bytes = ib;
                    }
                    else
                    {
                        out->inline_data = NULL;
                        out->inline_bytes = 0;
                    }
                    return true;
                }

                /* Only true inline blobs occupy bytes after the entry
                 * array; coalesced entries store nothing inline. */
                if (!(ib & PG_TRE_COALESCED_FLAG))
                    inline_offset += ib;
            }
        }

        /* Not found. */
        UnlockReleaseBuffer(buf);
        out->upper_buf = InvalidBuffer;
        out->owns_inline = false;
        return false;
    }
}

void
pg_tre_upper_release(PgTreUpperRef *ref)
{
    if (BufferIsValid(ref->upper_buf))
        UnlockReleaseBuffer(ref->upper_buf);
    if (ref->owns_inline && ref->inline_data != NULL)
    {
        pfree((void *) ref->inline_data);
        ref->inline_data = NULL;
        ref->owns_inline = false;
    }
}
