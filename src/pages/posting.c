/*
 * src/pages/posting.c - per-trigram posting tree (sparsemap-backed).
 *
 * Phase 1: page layout, leaf sparsemap accessors.
 * Phase 2: bulk loader (streaming sorted (trigram,tid) pairs).
 * Phase 3: scan iterator + sparsemap_intersection / union driver.
 * Phase 4: incremental insert and delete.
 *
 * ---- Phase 1/2 implementation ----
 *
 * Builder API:
 *   - Accumulates (TID, positions, bloom) tuples in memory using sparsemap
 *   - Flushes to disk when sparsemap serialization exceeds leaf budget
 *   - Emits WAL records for each leaf page written
 *   - Returns either inline blob or posting-tree root block
 *
 * Layout:
 *   - Leaf pages: PgTrePostingLeafHeader + sparsemap blob + payload area
 *   - Internal pages: (min_tid, child_blk) entries (Phase 4)
 *   - Lehman-Yao right-links for concurrent scans (Phase 3)
 *
 * For Phase 2, positions and tuple_bloom are NULL; payload handling
 * is stubbed for Phase 5.
 */

#include "postgres.h"

#include "access/xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lockdefs.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "pg_tre/buffer.h"
#include "pg_tre/page.h"
#include "pg_tre/posting.h"
#include "pg_tre/sparsemap.h"
#include "pg_tre/xlog.h"

/* Builder state for one trigram's posting tree. */
struct PgTrePostingBuilder
{
    Relation    index;
    uint64      trigram_hash;
    bool        with_payload;
    MemoryContext mcxt;

    /* In-memory accumulator. */
    sparsemap_t *map;
    uint8       *map_buffer;
    Size        map_buffer_size;

    /* For multi-leaf posting trees. */
    BlockNumber *leaf_blocks;
    int         n_leaves;
    int         leaves_alloced;

    uint64      min_tid;
    uint64      max_tid;
};

/* Scan state for iterating over a posting tree. */
struct PgTrePostingScan
{
    Relation    index;
    BlockNumber root;
    const uint8 *inline_data;
    Size        inline_bytes;

    /* Current leaf. */
    Buffer      leaf_buf;
    sparsemap_t *leaf_map;
    BlockNumber next_leaf;

    /* For inline postings, we wrap the data in-place. */
    sparsemap_t inline_map_storage;
};

/* Default sparsemap buffer size for builder (will grow if needed). */
#define POSTING_BUILDER_INITIAL_SIZE (8 * 1024)

/* Maximum sparsemap bytes that fit in one leaf page. */
#define POSTING_LEAF_MAX_SPARSEMAP_BYTES \
    (BLCKSZ - sizeof(PageHeaderData) - sizeof(PgTrePostingLeafHeader) - \
     sizeof(PageTreOpaqueData) - 64 /* safety margin */)

/* ---- Phase 1/2: Builder API ---- */

PgTrePostingBuilder *
pg_tre_posting_build_begin(Relation index, uint64 trigram_hash,
                           bool with_payload)
{
    PgTrePostingBuilder *b;
    MemoryContext oldcxt;

    b = (PgTrePostingBuilder *) palloc0(sizeof(PgTrePostingBuilder));
    b->mcxt = AllocSetContextCreate(CurrentMemoryContext,
                                    "posting builder context",
                                    ALLOCSET_DEFAULT_SIZES);
    oldcxt = MemoryContextSwitchTo(b->mcxt);

    b->index = index;
    b->trigram_hash = trigram_hash;
    b->with_payload = with_payload;

    /* Allocate initial sparsemap buffer. */
    b->map_buffer_size = POSTING_BUILDER_INITIAL_SIZE;
    b->map_buffer = (uint8 *) palloc(b->map_buffer_size);
    b->map = sparsemap_wrap(b->map_buffer, b->map_buffer_size);
    if (!b->map)
        elog(ERROR, "pg_tre: sparsemap_wrap failed");

    b->leaf_blocks = NULL;
    b->n_leaves = 0;
    b->leaves_alloced = 0;
    b->min_tid = UINT64_MAX;
    b->max_tid = 0;

    MemoryContextSwitchTo(oldcxt);
    return b;
}

void
pg_tre_posting_build_add(PgTrePostingBuilder *b, ItemPointer tid,
                         const uint32 *positions, int n_positions,
                         const uint8 *tuple_bloom_bits)
{
    uint64  packed_tid;
    Size    used;

    packed_tid = pg_tre_pack_tid(tid);

    /*
     * Phase 2 ignores positions and tuple_bloom; Phase 5 stores them
     * in the payload region.  For now, just accumulate TIDs in the
     * sparsemap.
     */
    (void) positions;
    (void) n_positions;
    (void) tuple_bloom_bits;

retry:
    if (sparsemap_add(b->map, packed_tid) == SPARSEMAP_IDX_MAX)
    {
        /* Buffer is full; grow it and retry. */
        Size new_size = b->map_buffer_size * 2;
        uint8 *new_buf = (uint8 *) repalloc(b->map_buffer, new_size);
        b->map_buffer = new_buf;
        b->map_buffer_size = new_size;
        b->map = sparsemap_set_data_size(b->map, b->map_buffer, new_size);
        if (!b->map)
            elog(ERROR, "pg_tre: sparsemap_set_data_size failed");
        goto retry;
    }

    /* Track TID range for leaf header. */
    if (packed_tid < b->min_tid)
        b->min_tid = packed_tid;
    if (packed_tid > b->max_tid)
        b->max_tid = packed_tid;

    /*
     * Check if sparsemap serialization exceeds the leaf budget.  If
     * so, flush current map to a leaf page and start a new one.
     */
    used = sparsemap_get_size(b->map);
    if (used > POSTING_LEAF_MAX_SPARSEMAP_BYTES)
    {
        /*
         * TODO: flush current sparsemap to a leaf page.  For Phase 2
         * bulk build, we collect all TIDs first, then flush once in
         * finish().  Multi-leaf flushing is Phase 4.
         */
    }
}

/*
 * Flush the current sparsemap to a new leaf page, WAL-log it, and
 * record the block number.
 */
static void
posting_flush_leaf(PgTrePostingBuilder *b)
{
    Buffer  buf;
    Page    page;
    PgTrePostingLeafHeader *hdr;
    char   *sparsemap_dest;
    Size    map_bytes;

    /* Extend the index to allocate a new leaf page. */
    buf = pg_tre_extend(b->index, PG_TRE_PAGE_POSTING_L);
    page = BufferGetPage(buf);

    /* Initialize leaf header. */
    hdr = (PgTrePostingLeafHeader *) PageGetContents(page);
    hdr->right_link = InvalidBlockNumber;
    hdr->min_tid = b->min_tid;
    hdr->max_tid = b->max_tid;
    hdr->n_entries = (uint16) sparsemap_cardinality(b->map);

    /* Copy sparsemap blob after header. */
    map_bytes = sparsemap_get_size(b->map);
    hdr->sparsemap_bytes = (uint32) map_bytes;
    sparsemap_dest = (char *) hdr + sizeof(PgTrePostingLeafHeader);
    memcpy(sparsemap_dest, sparsemap_get_data(b->map), map_bytes);

    /* No payload for Phase 2. */
    hdr->payload_bytes = 0;
    hdr->payload_offset = 0;

    /* Update pd_lower to reflect used space. */
    ((PageHeader) page)->pd_lower =
        (char *) sparsemap_dest + map_bytes - (char *) page;

    /* WAL-log as a full-page image. */
    if (RelationNeedsWAL(b->index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT | REGBUF_STANDARD);

        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_POSTING_INSERT);
        PageSetLSN(page, recptr);
    }

    MarkBufferDirty(buf);

    /* Record the leaf block. */
    if (b->n_leaves >= b->leaves_alloced)
    {
        int new_alloced = b->leaves_alloced == 0 ? 8 : b->leaves_alloced * 2;
        b->leaf_blocks = (BlockNumber *) repalloc(b->leaf_blocks,
                                                   new_alloced * sizeof(BlockNumber));
        b->leaves_alloced = new_alloced;
    }
    b->leaf_blocks[b->n_leaves++] = BufferGetBlockNumber(buf);

    UnlockReleaseBuffer(buf);
}

BlockNumber
pg_tre_posting_build_finish(PgTrePostingBuilder *b,
                            const uint8 **inline_data_out,
                            Size *inline_bytes_out)
{
    Size map_bytes = sparsemap_get_size(b->map);

    /*
     * If the serialized sparsemap is small enough, return it inline
     * rather than allocating a posting-tree page.
     */
    if (map_bytes <= PG_TRE_INLINE_POSTING_MAX)
    {
        *inline_data_out = (const uint8 *) sparsemap_get_data(b->map);
        *inline_bytes_out = map_bytes;
        return InvalidBlockNumber;
    }

    /*
     * Flush the accumulated sparsemap to a leaf page.  For Phase 2,
     * single-leaf postings are the common case.  Multi-leaf trees
     * with internal pages are Phase 4.
     */
    posting_flush_leaf(b);

    if (b->n_leaves == 1)
    {
        /* Single leaf: return its block number directly. */
        *inline_data_out = NULL;
        *inline_bytes_out = 0;
        return b->leaf_blocks[0];
    }
    else
    {
        /*
         * Multi-leaf posting tree requires internal pages.  Phase 4
         * builds the internal B-tree here.  For Phase 2, this is rare
         * enough that we can just error out with a clear message.
         */
        elog(ERROR, "pg_tre: multi-leaf posting trees not yet implemented "
             "(Phase 4); trigram %016" INT64_MODIFIER "X has %d leaves",
             b->trigram_hash, b->n_leaves);
    }
}

void
pg_tre_posting_build_free(PgTrePostingBuilder *b)
{
    if (b->map)
        free(b->map);
    MemoryContextDelete(b->mcxt);
}

/* ---- Phase 3: Scan API ---- */

PgTrePostingScan *
pg_tre_posting_scan_begin(Relation index, BlockNumber root,
                          const uint8 *inline_data, Size inline_bytes)
{
    PgTrePostingScan *s;

    s = (PgTrePostingScan *) palloc0(sizeof(PgTrePostingScan));
    s->index = index;
    s->root = root;
    s->inline_data = inline_data;
    s->inline_bytes = inline_bytes;
    s->leaf_buf = InvalidBuffer;
    s->leaf_map = NULL;
    s->next_leaf = root;

    return s;
}

bool
pg_tre_posting_scan_next(PgTrePostingScan *s, sparsemap_t **out,
                         BlockNumber *min_tid_blk, BlockNumber *max_tid_blk)
{
    /* Phase 3 implements leaf iteration; Phase 2 only needs materialize. */
    elog(ERROR, "pg_tre: posting scan iteration not yet implemented (Phase 3)");
}

sparsemap_t *
pg_tre_posting_materialize(Relation index, BlockNumber root,
                           const uint8 *inline_data, Size inline_bytes)
{
    if (inline_data != NULL)
    {
        /* Inline posting: wrap the data in-place and copy. */
        sparsemap_t inline_map_storage;
        sparsemap_open(&inline_map_storage, (uint8 *) inline_data, inline_bytes);
        return sparsemap_copy(&inline_map_storage);
    }
    else if (root != InvalidBlockNumber)
    {
        /* Single-leaf posting: read the leaf and copy its sparsemap. */
        Buffer  buf;
        Page    page;
        PgTrePostingLeafHeader *hdr;
        uint8  *map_data;
        sparsemap_t leaf_map;
        sparsemap_t *result;

        buf = pg_tre_read(index, root, PG_TRE_PAGE_POSTING_L,
                          BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);
        hdr = (PgTrePostingLeafHeader *) PageGetContents(page);
        map_data = (uint8 *) hdr + sizeof(PgTrePostingLeafHeader);

        /* Wrap and copy. */
        sparsemap_open(&leaf_map, map_data, hdr->sparsemap_bytes);
        result = sparsemap_copy(&leaf_map);

        UnlockReleaseBuffer(buf);
        return result;
    }
    else
    {
        /* Empty posting. */
        return NULL;
    }
}

void
pg_tre_posting_scan_end(PgTrePostingScan *s)
{
    if (BufferIsValid(s->leaf_buf))
        UnlockReleaseBuffer(s->leaf_buf);
    if (s->leaf_map)
        free(s->leaf_map);
    pfree(s);
}

/* ---- Upper-tree lookup (both sides) ---- */

bool
pg_tre_upper_lookup(Relation index, uint64 trigram_hash, PgTreUpperRef *out)
{
    /*
     * Phase 2 implements this by reading the upper tree and searching
     * for the trigram_hash.  For now, stub it to return "not found".
     */
    out->upper_buf = InvalidBuffer;
    out->root = InvalidBlockNumber;
    out->inline_data = NULL;
    out->inline_bytes = 0;
    return false;
}

void
pg_tre_upper_release(PgTreUpperRef *ref)
{
    if (BufferIsValid(ref->upper_buf))
        UnlockReleaseBuffer(ref->upper_buf);
}
