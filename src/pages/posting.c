/*
 * src/pages/posting.c - posting-tree read/write implementation.
 *
 * Phase 2 scope: single-leaf posting trees.  Per-trigram TID sets are
 * accumulated in an in-memory sparsemap (malloc-backed, grown by
 * sparsemap's internal realloc).  On finish():
 *   - If the serialized bitmap is <= PG_TRE_INLINE_POSTING_MAX bytes,
 *     we return it as an inline blob (palloc'd in CurrentMemoryContext
 *     so it outlives the builder).
 *   - Otherwise we allocate a single leaf page, copy the sparsemap
 *     bytes into it, WAL-log an FPI XLOG_PTRE_POSTING_INSERT, and
 *     return the leaf's block number as the posting root.
 *
 * Multi-leaf posting trees (B-tree-of-leaves with right-links for
 * Lehman-Yao scans) are a Phase 4 follow-up.  Today, if a single
 * trigram's posting does not fit in one leaf, the builder raises
 * ERROR with a clear HINT.  This is acceptable for Phase 2 fixture
 * sizes (~1k rows per fixture; sparsemap RLE typically keeps
 * postings tiny).
 *
 * The reader side mirrors this: materialize() and scan_begin() read
 * either the inline blob or a single leaf; multi-leaf walks wait for
 * Phase 4.
 */

#include "postgres.h"

#include <inttypes.h>
#include <string.h>

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

/* Usable bytes per posting leaf for the sparsemap blob. */
static inline Size
posting_leaf_budget(void)
{
    return BLCKSZ
         - MAXALIGN(SizeOfPageHeaderData)
         - MAXALIGN(sizeof(PgTrePostingLeafHeader))
         - MAXALIGN(sizeof(PageTreOpaqueData));
}

/* ================================================================
 * Builder
 * ================================================================ */

struct PgTrePostingBuilder
{
    Relation        index;
    uint64          trigram_hash;
    bool            with_payload;
    sparsemap_t    *smap;           /* malloc-backed */
    uint64          min_tid;
    uint64          max_tid;
    uint64          n_tids;
};

PgTrePostingBuilder *
pg_tre_posting_build_begin(Relation index, uint64 trigram_hash,
                           bool with_payload)
{
    PgTrePostingBuilder *b;

    b = (PgTrePostingBuilder *) palloc0(sizeof(*b));
    b->index        = index;
    b->trigram_hash = trigram_hash;
    b->with_payload = with_payload;
    b->min_tid      = UINT64_MAX;
    b->max_tid      = 0;
    b->n_tids       = 0;

    /* Start with a reasonable-sized buffer; sparsemap will grow on demand. */
    b->smap = sparsemap(1024);
    if (b->smap == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("pg_tre: failed to allocate sparsemap for posting builder")));

    return b;
}

void
pg_tre_posting_build_add(PgTrePostingBuilder *b, ItemPointer tid,
                         const uint32 *positions, int n_positions,
                         const uint8 *tuple_bloom_bits)
{
    uint64 packed = pg_tre_pack_tid(tid);
    uint64 rc;

    /* Phase 2 ignores payload (positions, bloom) -- Phase 5 restores. */
    (void) positions;
    (void) n_positions;
    (void) tuple_bloom_bits;

    rc = sparsemap_add(b->smap, packed);

    /*
     * sparsemap_add returns the index on success, or SPARSEMAP_IDX_MAX
     * to signal out-of-space.  When out-of-space, grow the buffer and
     * retry.
     */
    if (rc == SPARSEMAP_IDX_MAX)
    {
        /*
         * Double the buffer via sparsemap_set_data_size(map, NULL, new_size).
         * The sparsemap owns its buffer (allocated by sparsemap()), so a
         * NULL data argument triggers an internal realloc.
         */
        size_t cur = sparsemap_get_capacity(b->smap);
        size_t nxt = cur == 0 ? 1024 : cur * 2;
        sparsemap_t *grown = sparsemap_set_data_size(b->smap, NULL, nxt);
        if (grown == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("pg_tre: failed to grow posting sparsemap")));
        b->smap = grown;
        rc = sparsemap_add(b->smap, packed);
        if (rc == SPARSEMAP_IDX_MAX)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("pg_tre: sparsemap_add failed after grow")));
    }

    if (packed < b->min_tid) b->min_tid = packed;
    if (packed > b->max_tid) b->max_tid = packed;
    b->n_tids++;
}

/*
 * Allocate a single posting leaf page and write the sparsemap blob
 * into it, WAL-logged as a full-page image.  Returns the leaf's block
 * number.
 */
static BlockNumber
write_single_leaf(Relation index, uint64 trigram_hash,
                  const uint8 *data, Size sz,
                  uint64 min_tid, uint64 max_tid, uint64 n_tids)
{
    Buffer  buf;
    Page    page;
    PgTrePostingLeafHeader *hdr;
    BlockNumber blkno;
    char   *payload_area;

    if (sz > posting_leaf_budget())
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("pg_tre: posting for trigram %016" PRIx64 " "
                        "is %zu bytes, exceeds single-leaf budget %zu",
                        trigram_hash, sz, posting_leaf_budget()),
                 errhint("Multi-leaf posting trees land in a Phase 4 "
                         "follow-up; reduce fixture size or rebuild after "
                         "that work completes.")));

    buf   = pg_tre_extend(index, PG_TRE_PAGE_POSTING_L);
    page  = BufferGetPage(buf);
    blkno = BufferGetBlockNumber(buf);

    /* PgTrePostingLeafHeader lives right after the PageHeader, before the
     * line-pointer area (we don't use line pointers on this page).  Store
     * it at the start of the lower-free region. */
    hdr = (PgTrePostingLeafHeader *) PageGetContents(page);
    hdr->right_link      = InvalidBlockNumber;
    hdr->min_tid         = min_tid;
    hdr->max_tid         = max_tid;
    hdr->sparsemap_bytes = (uint32) sz;
    hdr->payload_bytes   = 0;
    hdr->payload_offset  = 0;
    hdr->n_entries       = (n_tids > UINT16_MAX) ? UINT16_MAX : (uint16) n_tids;
    hdr->_pad            = 0;

    payload_area = (char *) hdr + MAXALIGN(sizeof(*hdr));
    memcpy(payload_area, data, sz);

    /* Advance pd_lower past our content so PageAddItem sees a
     * self-consistent page if we ever add line-pointered entries. */
    ((PageHeader) page)->pd_lower =
        (payload_area + sz) - (char *) page;

    MarkBufferDirty(buf);

    if (RelationNeedsWAL(index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_POSTING_INSERT);
        PageSetLSN(page, recptr);
    }

    UnlockReleaseBuffer(buf);
    return blkno;
}

BlockNumber
pg_tre_posting_build_finish(PgTrePostingBuilder *b,
                            const uint8 **inline_data_out,
                            Size *inline_bytes_out)
{
    Size    sz = sparsemap_get_size(b->smap);
    /*
     * sparsemap_get_size reports only the actively-used bytes; we need a
     * pointer to the underlying buffer.  The sparsemap API doesn't
     * directly expose it, so use sparsemap_copy to obtain a contiguous
     * serialized image in a known buffer, then extract the bytes via a
     * second wrap.  For simplicity we recompute via scan & rebuild: the
     * easier path is to treat the current map as already-serialized and
     * read through sparsemap_open to inspect its byte layout.
     *
     * In practice, sparsemap's internal buffer IS the serialization --
     * the struct carries a data pointer.  But since the struct is opaque
     * in the header, we reach the bytes by re-initializing a stack map
     * over the same storage -- except we don't have access to the
     * pointer.  Work around by serializing to a fresh buffer we control.
     */
    const uint8 *bytes;
    uint8      *copy;

    /*
     * Round-trip through sparsemap_wrap to get a stable byte image that
     * we own: copy the map, capture its size, then extract bytes through
     * a wrapping handle over a palloc'd buffer.
     *
     * Simplest correct approach: allocate `sz` bytes in CurrentMemoryContext,
     * initialize a fresh wrapped sparsemap over it, iterate b->smap via
     * sparsemap_scan and re-add into the new map.  This is O(n_tids) but
     * n_tids is bounded by leaf capacity.  Since ambuild's finish path
     * happens once per trigram, cost is acceptable for Phase 2.
     */
    {
        /* Ensure we request at least a 1-byte buffer even for empty maps. */
        Size cap = (sz == 0) ? 16 : sz + 16;
        copy = (uint8 *) palloc0(cap);
        sparsemap_t *fresh;
        uint64 idx;
        uint64 min_idx = sparsemap_minimum(b->smap);
        uint64 max_idx = sparsemap_maximum(b->smap);

        fresh = sparsemap_wrap(copy, cap);
        if (fresh == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("pg_tre: failed to wrap sparsemap")));

        if (b->n_tids > 0)
        {
            for (idx = min_idx; idx <= max_idx; idx++)
            {
                if (sparsemap_contains(b->smap, idx))
                {
                    uint64 rc = sparsemap_add(fresh, idx);
                    if (rc == SPARSEMAP_IDX_MAX)
                        ereport(ERROR,
                                (errcode(ERRCODE_INTERNAL_ERROR),
                                 errmsg("pg_tre: re-serialize to flat blob failed")));
                }
                if (idx == max_idx) break; /* guard against wrap */
            }
        }
        sz = sparsemap_get_size(fresh);
        free(fresh);           /* free only the handle; we own `copy` */
        bytes = copy;
    }

    if (sz <= PG_TRE_INLINE_POSTING_MAX)
    {
        *inline_data_out  = bytes;      /* palloc'd; outlives builder */
        *inline_bytes_out = sz;
        return InvalidBlockNumber;
    }

    {
        BlockNumber blk = write_single_leaf(b->index, b->trigram_hash,
                                            bytes, sz,
                                            b->min_tid, b->max_tid,
                                            b->n_tids);
        /* When stored on-disk, we don't need the palloc'd copy. */
        pfree(copy);
        *inline_data_out  = NULL;
        *inline_bytes_out = 0;
        return blk;
    }
}

void
pg_tre_posting_build_free(PgTrePostingBuilder *b)
{
    if (b == NULL)
        return;
    if (b->smap != NULL)
        free(b->smap);
    pfree(b);
}

/* ================================================================
 * Reader
 * ================================================================ */

struct PgTrePostingScan
{
    Relation    index;
    BlockNumber cur_blk;       /* InvalidBlockNumber when done */
    const uint8 *inline_data;
    Size        inline_bytes;
    bool        served_inline;
    Buffer      pinned_buf;    /* pinned while *smap is live */
    sparsemap_t *smap;
};

PgTrePostingScan *
pg_tre_posting_scan_begin(Relation index, BlockNumber root,
                          const uint8 *inline_data, Size inline_bytes)
{
    PgTrePostingScan *s = (PgTrePostingScan *) palloc0(sizeof(*s));
    s->index       = index;
    s->cur_blk     = root;
    s->inline_data = inline_data;
    s->inline_bytes= inline_bytes;
    s->served_inline = false;
    s->pinned_buf  = InvalidBuffer;
    s->smap        = NULL;
    return s;
}

static void
scan_release_current(PgTrePostingScan *s)
{
    if (s->smap != NULL)
    {
        free(s->smap);
        s->smap = NULL;
    }
    if (BufferIsValid(s->pinned_buf))
    {
        UnlockReleaseBuffer(s->pinned_buf);
        s->pinned_buf = InvalidBuffer;
    }
}

bool
pg_tre_posting_scan_next(PgTrePostingScan *s, sparsemap_t **out,
                         BlockNumber *min_tid_blk, BlockNumber *max_tid_blk)
{
    scan_release_current(s);

    /* Inline case: serve exactly once. */
    if (s->inline_data != NULL && !s->served_inline)
    {
        /* Wrap over a palloc'd copy so we own stable storage. */
        uint8 *buf = (uint8 *) palloc(s->inline_bytes);
        memcpy(buf, s->inline_data, s->inline_bytes);
        s->smap = sparsemap_wrap(buf, s->inline_bytes);
        s->served_inline = true;
        *out = s->smap;
        if (min_tid_blk) *min_tid_blk = 0;
        if (max_tid_blk) *max_tid_blk = InvalidBlockNumber;
        return true;
    }

    if (!BlockNumberIsValid(s->cur_blk))
        return false;

    {
        Buffer  buf = pg_tre_read(s->index, s->cur_blk, PG_TRE_PAGE_POSTING_L,
                                  BUFFER_LOCK_SHARE);
        Page    page = BufferGetPage(buf);
        PgTrePostingLeafHeader *hdr =
            (PgTrePostingLeafHeader *) PageGetContents(page);
        uint8  *sm_bytes = (uint8 *) hdr + MAXALIGN(sizeof(*hdr));
        uint8  *copy;

        /* Copy bytes out so the caller's sparsemap survives unlock. */
        copy = (uint8 *) palloc(hdr->sparsemap_bytes + 8);
        memcpy(copy, sm_bytes, hdr->sparsemap_bytes);

        s->smap = sparsemap_wrap(copy, hdr->sparsemap_bytes);
        s->pinned_buf = InvalidBuffer;   /* already copied; release */
        UnlockReleaseBuffer(buf);

        if (min_tid_blk)
            *min_tid_blk = (BlockNumber) (hdr->min_tid >> 16);
        if (max_tid_blk)
            *max_tid_blk = (BlockNumber) (hdr->max_tid >> 16);

        s->cur_blk = hdr->right_link;
        *out = s->smap;
        return true;
    }
}

void
pg_tre_posting_scan_end(PgTrePostingScan *s)
{
    if (s == NULL)
        return;
    scan_release_current(s);
    pfree(s);
}

sparsemap_t *
pg_tre_posting_materialize(Relation index, BlockNumber root,
                           const uint8 *inline_data, Size inline_bytes)
{
    /*
     * Phase 2: single-leaf posting.  The materialized sparsemap is a
     * palloc'd copy of the stored bytes, wrapped.  For multi-leaf
     * postings (Phase 4), this function will union leaves into one
     * accumulating sparsemap before returning.
     */
    if (inline_data != NULL)
    {
        Size cap = inline_bytes + 8;
        uint8 *copy = (uint8 *) palloc(cap);
        memcpy(copy, inline_data, inline_bytes);
        /* Zero any slack bytes so sparsemap_open sees a clean tail. */
        if (cap > inline_bytes)
            memset(copy + inline_bytes, 0, cap - inline_bytes);
        return sparsemap_wrap(copy, cap);
    }

    if (BlockNumberIsValid(root))
    {
        Buffer  buf = pg_tre_read(index, root, PG_TRE_PAGE_POSTING_L,
                                  BUFFER_LOCK_SHARE);
        Page    page = BufferGetPage(buf);
        PgTrePostingLeafHeader *hdr =
            (PgTrePostingLeafHeader *) PageGetContents(page);
        uint8  *sm_bytes = (uint8 *) hdr + MAXALIGN(sizeof(*hdr));
        Size    cap = hdr->sparsemap_bytes + 8;
        uint8  *copy = (uint8 *) palloc(cap);
        sparsemap_t *sm;

        memcpy(copy, sm_bytes, hdr->sparsemap_bytes);
        memset(copy + hdr->sparsemap_bytes, 0, cap - hdr->sparsemap_bytes);
        sm = sparsemap_wrap(copy, cap);

        /* Phase 2: no right-link traversal (single-leaf postings). */
        UnlockReleaseBuffer(buf);
        return sm;
    }

    /* Empty posting: a 16-byte zeroed sparsemap is a valid empty map. */
    {
        uint8 *zero = (uint8 *) palloc0(16);
        return sparsemap_wrap(zero, 16);
    }
}
