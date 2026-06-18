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
#include <limits.h>
#include <string.h>

#include "access/genam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/lockdefs.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "pg_tre/buffer.h"
#include "pg_tre/coalesced.h"
#include "pg_tre/meta.h"
#include "pg_tre/page.h"
#include "pg_tre/posting.h"
#include "pg_tre/pg_tre.h"
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

/*
 * Validate a posting-leaf header's self-describing size/offset fields
 * against the physical page budget.  A corrupt or truncated page (e.g.
 * after a torn write or hostile input) could otherwise drive memcpy /
 * sparsemap walks past the end of the buffer.  Returns true if the
 * header is internally consistent and all regions fit within the page.
 *
 * Layout invariants (see page.h):
 *   header end  = MAXALIGN(SizeOfPageHeaderData) + MAXALIGN(sizeof(hdr))
 *   sparsemap   occupies [header_end, header_end + sparsemap_bytes)
 *   payload     occupies [payload_offset, payload_offset + payload_bytes)
 *   everything  must stay below BLCKSZ - MAXALIGN(sizeof(PageTreOpaqueData))
 */
static inline bool
posting_leaf_header_valid(const PgTrePostingLeafHeader *hdr)
{
    Size header_end = MAXALIGN(SizeOfPageHeaderData)
                    + MAXALIGN(sizeof(PgTrePostingLeafHeader));
    Size usable_end = BLCKSZ - MAXALIGN(sizeof(PageTreOpaqueData));

    if (hdr->sparsemap_bytes > posting_leaf_budget())
        return false;

    if (hdr->payload_bytes > 0 || hdr->payload_offset != 0)
    {
        /* payload region, when present, must start past the header and
         * fully fit before the opaque trailer */
        if (hdr->payload_offset < header_end)
            return false;
        if (hdr->payload_offset > usable_end)
            return false;
        if (hdr->payload_bytes > usable_end - hdr->payload_offset)
            return false;
    }

    return true;
}

/* ================================================================
 * Page deletion / recycle (FSM page-freeing, issue: emptied posting
 * leaves were repacked but never physically reclaimed).
 *
 * Posting trees are flat right-link leaf chains.  When VACUUM empties a
 * non-head leaf it is spliced out of the chain (left sibling's
 * right_link is advanced past it) and marked PG_TRE_LEAF_DELETED, but
 * NOT immediately freed: a concurrent scan may have copied a stale
 * right_link pointing at it before the unlink (scans follow right_link
 * without lock coupling -- see pg_tre_posting_scan_next).  The deleted
 * page therefore stays a coherent waypoint -- empty sparsemap (matches
 * no TIDs), preserved right_link (a stale scanner continues correctly to
 * the real successor).  We stamp the full XID at unlink time; a later
 * VACUUM recycles the page into the index FSM only once that XID is old
 * enough that no snapshot could still be mid-traversal (nbtree's safexid
 * discipline, GlobalVisCheckRemovableFullXid).
 * ================================================================ */

/*
 * Offset within a deleted posting leaf where the 8-byte deletion XID is
 * stored: immediately past the leaf header and the (empty) sparsemap
 * blob.  A normal-leaf reader never looks here (it reads only
 * sparsemap_bytes of sparsemap and, for a deleted page, payload_bytes ==
 * 0), so the stamp is invisible to stale scanners that still treat the
 * page as a live leaf.
 */
static inline Size
posting_deleted_xid_offset(const PgTrePostingLeafHeader *hdr)
{
    Size header_end = MAXALIGN(SizeOfPageHeaderData)
                    + MAXALIGN(sizeof(PgTrePostingLeafHeader));
    return header_end + MAXALIGN(hdr->sparsemap_bytes);
}

/* Serialize an empty sparsemap into a freshly-palloc'd buffer; returns
 * the blob and writes its size.  Used to turn a leaf into a coherent
 * empty waypoint. */
static uint8 *
posting_make_empty_sparsemap(Size *out_size)
{
    sm_t *sm = sm_create(64);
    uint8  *blob;
    Size    sz;

    if (sm == NULL)
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
            errmsg("pg_tre: failed to allocate empty sparsemap")));
    sz = sm_get_size(sm);
    blob = (uint8 *) palloc(sz + 16);
    memcpy(blob, sm_get_data(sm), sz);
    sm_free(sm);
    *out_size = sz;
    return blob;
}

/*
 * Unlink an emptied leaf `cur` (held EXCLUSIVE in cur_buf) from its
 * predecessor `prev` (held EXCLUSIVE in prev_buf).  After the call:
 *   - prev->right_link == cur->right_link (cur is out of the chain),
 *   - cur is rewritten as an empty waypoint with PG_TRE_LEAF_DELETED set
 *     and a deletion XID stamped,
 * and both pages are WAL-logged as a single 2-FPI record.  Both buffers
 * remain locked and pinned (caller releases).
 */
static void
posting_leaf_unlink(Relation index, Buffer prev_buf, Buffer cur_buf,
                    BlockNumber cur_blk)
{
    Page    prev_page = BufferGetPage(prev_buf);
    Page    cur_page  = BufferGetPage(cur_buf);
    PgTrePostingLeafHeader *prev_hdr =
        (PgTrePostingLeafHeader *) PageGetContents(prev_page);
    PgTrePostingLeafHeader *cur_hdr =
        (PgTrePostingLeafHeader *) PageGetContents(cur_page);
    PageTreOpaque cur_opq = PageTreGetOpaque(cur_page);
    BlockNumber cur_right = cur_hdr->right_link;
    FullTransactionId del_xid = ReadNextFullTransactionId();
    uint8  *empty_sm;
    Size    empty_sm_sz;
    char   *sm_area;
    Size    xid_off;

    /* 1. Splice cur out of the chain. */
    prev_hdr->right_link = cur_right;

    /* 2. Rewrite cur as an empty, still-linked waypoint. */
    empty_sm = posting_make_empty_sparsemap(&empty_sm_sz);

    cur_hdr->right_link     = cur_right;     /* preserved for stale scanners */
    cur_hdr->min_tid        = 0;
    cur_hdr->max_tid        = 0;
    cur_hdr->sparsemap_bytes = (uint32) empty_sm_sz;
    cur_hdr->payload_bytes  = 0;
    cur_hdr->payload_offset = 0;
    cur_hdr->n_entries      = 0;
    cur_hdr->_pad           = 0;

    sm_area = (char *) cur_hdr + MAXALIGN(sizeof(*cur_hdr));
    memcpy(sm_area, empty_sm, empty_sm_sz);
    pfree(empty_sm);

    /* Stamp the deletion XID just past the empty sparsemap. */
    xid_off = posting_deleted_xid_offset(cur_hdr);
    Assert(xid_off + sizeof(uint64) <= (Size) (BLCKSZ - MAXALIGN(sizeof(PageTreOpaqueData))));
    memcpy((char *) cur_page + xid_off, &del_xid.value, sizeof(uint64));

    cur_opq->flags |= PG_TRE_LEAF_DELETED;

    /* pd_lower covers header + sparsemap + the 8-byte XID stamp so the
     * stamp survives REGBUF_STANDARD hole-stripping in the FPI; pd_upper
     * resets to the opaque trailer (no payload). */
    ((PageHeader) cur_page)->pd_lower = xid_off + sizeof(uint64);
    ((PageHeader) cur_page)->pd_upper =
        BLCKSZ - MAXALIGN(sizeof(PageTreOpaqueData));

    MarkBufferDirty(prev_buf);
    MarkBufferDirty(cur_buf);

    if (RelationNeedsWAL(index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, prev_buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        XLogRegisterBuffer(1, cur_buf,  REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_POSTING_UNLINK);
        PageSetLSN(prev_page, recptr);
        PageSetLSN(cur_page, recptr);
    }
    (void) cur_blk;
}

/*
 * Read the deletion XID of a leaf currently marked PG_TRE_LEAF_DELETED.
 */
static FullTransactionId
posting_leaf_deleted_xid(Page page)
{
    PgTrePostingLeafHeader *hdr =
        (PgTrePostingLeafHeader *) PageGetContents(page);
    FullTransactionId fxid;
    uint64  v;

    memcpy(&v, (char *) page + posting_deleted_xid_offset(hdr), sizeof(uint64));
    fxid.value = v;
    return fxid;
}

/* Per-TID payload entry (positions + bloom). */
typedef struct PayloadEntry
{
    uint64  tid;                /* packed TID */
    uint32 *positions;          /* palloc'd array of position offsets */
    int     n_positions;
    uint8  *bloom_bits;         /* palloc'd bloom filter bits */
} PayloadEntry;

struct PgTrePostingBuilder
{
    Relation        index;
    uint64          trigram_hash;
    bool            with_payload;

    /*
     * Phase 4.1: accumulate TIDs into a palloc'd dynamic array instead of
     * a dynamically-grown malloc-backed sparsemap.  The sparsemap dynamic
     * resize path (sm_set_data_size with data=NULL) triggers glibc
     * heap corruption for certain size transitions; palloc/repalloc is
     * solid.  The array is converted to a sparsemap once at finish time
     * when we know the final cardinality.
     */
    uint64         *tids;           /* palloc'd uint64 array */
    int             n_tids;
    int             tids_alloced;
    uint64          min_tid;
    uint64          max_tid;

    /* Payload tracking (Phase 5). */
    PayloadEntry   *payload;        /* palloc'd array */
    int             payload_count;
    int             payload_alloced;
};

PgTrePostingBuilder *
pg_tre_posting_build_begin(Relation index, uint64 trigram_hash,
                           bool with_payload)
{
    /* Default initial size: 1024 bytes.  Callers that know the TID count
     * up-front should call pg_tre_posting_build_begin_sized() instead to
     * avoid the dynamic-resize path through sm_set_data_size(),
     * which has a latent bug on repeated grows.  The sized variant is
     * used by the pending-list merge path where cardinality is known. */
    return pg_tre_posting_build_begin_sized(index, trigram_hash,
                                             with_payload, 1024);
}

PgTrePostingBuilder *
pg_tre_posting_build_begin_sized(Relation index, uint64 trigram_hash,
                                 bool with_payload, size_t expected_bytes)
{
    PgTrePostingBuilder *b;
    int initial_cap;

    /* Use expected_bytes as a hint for the uint64 array capacity:
     * each TID is 8 bytes in the array.  Minimum 64 slots. */
    initial_cap = expected_bytes / 8;
    if (initial_cap < 64) initial_cap = 64;

    b = (PgTrePostingBuilder *) palloc0(sizeof(*b));
    b->index        = index;
    b->trigram_hash = trigram_hash;
    b->with_payload = with_payload;
    b->min_tid      = UINT64_MAX;
    b->max_tid      = 0;
    b->n_tids       = 0;

    b->tids         = (uint64 *) palloc(initial_cap * sizeof(uint64));
    b->tids_alloced = initial_cap;

    /* Initialize payload array if enabled (Phase 5). */
    if (with_payload)
    {
        b->payload_alloced = 256;
        b->payload = (PayloadEntry *) palloc0(b->payload_alloced * sizeof(PayloadEntry));
        b->payload_count = 0;
    }
    else
    {
        b->payload = NULL;
        b->payload_count = 0;
        b->payload_alloced = 0;
    }

    return b;
}

void
pg_tre_posting_build_add(PgTrePostingBuilder *b, ItemPointer tid,
                         const uint32 *positions, int n_positions,
                         const uint8 *tuple_bloom_bits)
{
    uint64 packed = pg_tre_pack_tid(tid);

    /* Grow the uint64 array if needed (palloc-based, proven reliable). */
    if (b->n_tids >= b->tids_alloced)
    {
        if (b->tids_alloced > INT_MAX / 2)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("pg_tre: posting builder TID array too large")));
        b->tids_alloced *= 2;
        b->tids = (uint64 *) repalloc(b->tids,
                      (Size) b->tids_alloced * sizeof(uint64));
    }
    b->tids[b->n_tids] = packed;

    /* Phase 5: capture per-TID payload (positions + bloom). */
    if (b->with_payload)
    {
        PayloadEntry *pe;
        Size bloom_bytes;

        /* Grow payload array if needed. */
        if (b->payload_count >= b->payload_alloced)
        {
            if (b->payload_alloced > INT_MAX / 2)
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("pg_tre: posting builder payload array too large")));
            b->payload_alloced *= 2;
            b->payload = (PayloadEntry *) repalloc(b->payload,
                            (Size) b->payload_alloced * sizeof(PayloadEntry));
        }

        pe = &b->payload[b->payload_count++];
        pe->tid = packed;

        /*
         * The on-disk payload encodes n_positions as a uint16 (see
         * serialize_payload / the scan-side walk in
         * pg_tre_posting_lookup_positions).  Clamp here so a pathological
         * tuple cannot silently truncate the count on write-back and
         * desynchronize the payload walk on read.  Positions beyond the
         * cap are dropped; correctness is preserved because the executor
         * rechecks every emitted row, and the positional filter only
         * ever needs one in-range hit.
         */
        if (n_positions < 0)
            n_positions = 0;
        if (n_positions > UINT16_MAX)
            n_positions = UINT16_MAX;
        pe->n_positions = n_positions;

        /* Copy positions array. */
        if (n_positions > 0 && positions != NULL)
        {
            pe->positions = (uint32 *) palloc(n_positions * sizeof(uint32));
            memcpy(pe->positions, positions, n_positions * sizeof(uint32));
        }
        else
        {
            pe->positions = NULL;
        }

        /* Copy bloom bits. */
        bloom_bytes = (pg_tre_bloom_tuple_bits + 7) / 8;
        if (tuple_bloom_bits != NULL)
        {
            pe->bloom_bits = (uint8 *) palloc(bloom_bytes);
            memcpy(pe->bloom_bits, tuple_bloom_bits, bloom_bytes);
        }
        else
        {
            /* No bloom provided: allocate zeroed bloom. */
            pe->bloom_bits = (uint8 *) palloc0(bloom_bytes);
        }
    }

    if (packed < b->min_tid) b->min_tid = packed;
    if (packed > b->max_tid) b->max_tid = packed;
    b->n_tids++;
}

/*
 * Allocate a single posting leaf page and write the sparsemap blob
 * and optional payload into it, WAL-logged as a full-page image.
 * Returns the leaf's block number.
 *
 * Phase 5: payload_data is the serialized payload region (positions + blooms).
 */
static BlockNumber
write_single_leaf(Relation index, uint64 trigram_hash,
                  const uint8 *data, Size sz,
                  const uint8 *payload_data, Size payload_sz,
                  uint64 min_tid, uint64 max_tid, uint64 n_tids,
                  BlockNumber right_link)
{
    Buffer  buf;
    Page    page;
    PgTrePostingLeafHeader *hdr;
    BlockNumber blkno;
    char   *sparsemap_area;
    char   *payload_area;
    Size    total_sz;

    total_sz = sz + payload_sz;
    if (total_sz > posting_leaf_budget())
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("pg_tre: posting for trigram %016llx "
                        "is %zu bytes (sparsemap %zu + payload %zu), "
                        "exceeds single-leaf budget %zu",
                        (unsigned long long) trigram_hash, total_sz, sz, payload_sz,
                        posting_leaf_budget()),
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
    hdr->right_link      = right_link;
    hdr->min_tid         = min_tid;
    hdr->max_tid         = max_tid;
    hdr->sparsemap_bytes = (uint32) sz;
    hdr->n_entries       = (n_tids > UINT16_MAX) ? UINT16_MAX : (uint16) n_tids;
    hdr->_pad            = 0;

    sparsemap_area = (char *) hdr + MAXALIGN(sizeof(*hdr));
    memcpy(sparsemap_area, data, sz);

    if (payload_sz > 0 && payload_data != NULL)
    {
        /* Payload grows downward from the end of the page (before opaque). */
        payload_area = (char *) page + BLCKSZ
                     - MAXALIGN(sizeof(PageTreOpaqueData))
                     - payload_sz;
        memcpy(payload_area, payload_data, payload_sz);
        hdr->payload_bytes  = (uint32) payload_sz;
        hdr->payload_offset = payload_area - (char *) page;

        /* Advance pd_lower past sparsemap, pd_upper before payload. */
        ((PageHeader) page)->pd_lower = (sparsemap_area + sz) - (char *) page;
        ((PageHeader) page)->pd_upper = payload_area - (char *) page;
    }
    else
    {
        hdr->payload_bytes  = 0;
        hdr->payload_offset = 0;

        /* Advance pd_lower past our content so PageAddItem sees a
         * self-consistent page if we ever add line-pointered entries. */
        ((PageHeader) page)->pd_lower = (sparsemap_area + sz) - (char *) page;
    }

    MarkBufferDirty(buf);

    if (RelationNeedsWAL(index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_POSTING_INSERT);
        PageSetLSN(page, recptr);
    }

    UnlockReleaseBuffer(buf);
    return blkno;
}

/*
 * Serialize the payload array into a compact byte stream.
 * Format for each TID (in sparsemap order):
 *   - uint16 n_positions (variable-byte encoded as 1 or 2 bytes)
 *   - uint8  position_deltas[n_positions] (delta-coded from 0)
 *   - uint8  bloom_bits[(pg_tre_bloom_tuple_bits + 7) / 8]
 *
 * Phase 5: for simplicity, use fixed-width encoding: 2 bytes for n_positions,
 * 4 bytes per position (no delta coding yet), bloom_bits follow.
 *
 * Returns palloc'd buffer; caller must pfree.
 */
static uint8 *
serialize_payload(PayloadEntry *payload, int payload_count, Size *out_size)
{
    Size bloom_bytes = (pg_tre_bloom_tuple_bits + 7) / 8;
    Size estimate = 0;
    uint8 *buf, *ptr;
    int i;

    /* Estimate size: for each entry, 2 bytes for n_positions,
     * 4 bytes per position, and bloom_bytes for bloom. */
    for (i = 0; i < payload_count; i++)
    {
        estimate += 2;  /* n_positions */
        estimate += (Size) payload[i].n_positions * sizeof(uint32);
        estimate += bloom_bytes;
    }

    buf = (uint8 *) palloc0(estimate);
    ptr = buf;

    for (i = 0; i < payload_count; i++)
    {
        PayloadEntry *pe = &payload[i];
        uint16 n = (uint16) pe->n_positions;

        /* Write n_positions as uint16. */
        memcpy(ptr, &n, sizeof(uint16));
        ptr += sizeof(uint16);

        /* Write positions as uint32 array (no delta coding for Phase 5). */
        if (n > 0 && pe->positions != NULL)
        {
            memcpy(ptr, pe->positions, n * sizeof(uint32));
            ptr += n * sizeof(uint32);
        }

        /* Write bloom bits. */
        if (pe->bloom_bits != NULL)
        {
            memcpy(ptr, pe->bloom_bits, bloom_bytes);
        }
        else
        {
            /* Zero bloom if missing. */
            memset(ptr, 0, bloom_bytes);
        }
        ptr += bloom_bytes;
    }

    *out_size = ptr - buf;
    Assert(*out_size <= estimate);
    return buf;
}

BlockNumber
pg_tre_posting_build_finish(PgTrePostingBuilder *b,
                            const uint8 **inline_data_out,
                            Size *inline_bytes_out)
{
    bool        coalesced;
    BlockNumber cblk;
    uint16      cslot;

    return pg_tre_posting_build_finish_ex(b, inline_data_out, inline_bytes_out,
                                          NULL, &coalesced, &cblk, &cslot);
}

BlockNumber
pg_tre_posting_build_finish_ex(PgTrePostingBuilder *b,
                               const uint8 **inline_data_out,
                               Size *inline_bytes_out,
                               struct PgTreCoalescedWriter *cw,
                               bool *coalesced_out,
                               BlockNumber *cblk_out,
                               uint16 *cslot_out)
{
    const uint8 *bytes;
    uint8      *copy;
    uint8      *payload_bytes = NULL;
    Size        payload_sz = 0;
    Size        sz = 0;

    *coalesced_out = false;
    *cblk_out = InvalidBlockNumber;
    *cslot_out = 0;

    /*
     * Phase 4.1: convert the collected uint64 array of TIDs to a canonical
     * sparsemap serialization.  We allocate a fresh sparsemap with a
     * generous initial capacity (16 bytes per TID + 1 KiB overhead;
     * sparsemap RLE will compress further) and use sm_add_grow() so the
     * buffer doubles geometrically on ENOSPC.
     *
     * Why grow rather than pre-size: sparsemap's per-chunk overhead is
     * data-dependent (very sparse / scattered TID streams can incur a
     * fresh chunk header for every TID), so any static "bytes per TID"
     * estimate is wrong on some inputs.  On ~165k-row tables with a
     * regex-trigram-heavy column distribution we reproducibly hit
     * SM_IDX_MAX with the previous static 16 B/TID estimate; the
     * geometric grow eliminates this class of failure.  The serialized
     * on-disk bytes are unchanged — only the in-memory cap differs.
     */
    {
        size_t cap = (size_t) b->n_tids * 16 + 1024;
        sm_t *fresh = sm_create(cap);
        int k;
        if (fresh == NULL)
            ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
                errmsg("pg_tre: failed to allocate sparsemap for serialization")));
        for (k = 0; k < b->n_tids; k++)
        {
            int retries = 0;
            /* Make this loop cancellable; per-TID iterations are cheap
             * but n_tids can reach the millions on wide tables. */
            CHECK_FOR_INTERRUPTS();
            while (sm_add_grow(&fresh, b->tids[k]) == SM_IDX_MAX)
            {
                /*
                 * sm_add_grow already doubled the buffer once and
                 * retried.  If the retry still ENOSPCs (or the
                 * underlying allocation failed), call sm_add_grow
                 * again — each iteration doubles the buffer.  The
                 * 16-iteration cap (~65000× the initial size) is a
                 * safety bound: a real bug rather than a sparse-TID
                 * input would hit it.
                 */
                if (++retries > 16)
                {
                    size_t final_cap = sm_get_capacity(fresh);
                    sm_free(fresh);
                    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                        errmsg("pg_tre: sm_add_grow exhausted retries for tid[%d] (n_tids=%d, cap=%zu)",
                               k, b->n_tids, final_cap),
                        errhint("File a bug with the table size and column statistics.")));
                }
            }
        }
        sz = sm_get_size(fresh);
        {
            const uint8_t *smap_data = (const uint8_t *) sm_get_data(fresh);
            if (sz > 0)
            {
                copy = (uint8 *) palloc(sz + 16);
                memcpy(copy, smap_data, sz);
                memset(copy + sz, 0, 16);
            }
            else
            {
                copy = (uint8 *) palloc0(16);
            }
        }
        sm_free(fresh);
        bytes = copy;
    }

    /* Phase 5: serialize payload if present. */
    if (b->with_payload && b->payload_count > 0)
    {
        payload_bytes = serialize_payload(b->payload, b->payload_count,
                                         &payload_sz);
    }

    /* Inline case: sparsemap + payload must fit in PG_TRE_INLINE_POSTING_MAX. */
    if (sz + payload_sz <= PG_TRE_INLINE_POSTING_MAX)
    {
        if (payload_sz > 0)
        {
            /* Concatenate sparsemap + payload into a single inline blob. */
            uint8 *combined = (uint8 *) palloc(sz + payload_sz);
            memcpy(combined, bytes, sz);
            memcpy(combined + sz, payload_bytes, payload_sz);
            pfree(copy);
            if (payload_bytes)
                pfree(payload_bytes);
            *inline_data_out  = combined;
            *inline_bytes_out = sz + payload_sz;
        }
        else
        {
            *inline_data_out  = bytes;      /* palloc'd; outlives builder */
            *inline_bytes_out = sz;
        }
        return InvalidBlockNumber;
    }

    /* On-disk case: write sparsemap and payload to leaf page(s). */
    {
        Size total = sz + payload_sz;
        Size budget = posting_leaf_budget();

        /*
         * Coalescing (v2.0): a medium-bucket posting packs onto a
         * shared coalesced page instead of a dedicated single leaf.
         * Phase 1 drops the payload for coalesced postings (lossy-safe;
         * recheck is authoritative).  Only the sparsemap (sz) gates
         * eligibility, since that is what a coalesced slot stores.
         */
        if (cw != NULL
            && sz > PG_TRE_INLINE_POSTING_MAX
            && sz <= PG_TRE_COALESCE_MAX)
        {
            BlockNumber cblk;
            uint16      cslot;

            pg_tre_coalesced_add(cw, b->trigram_hash, bytes, sz,
                                 NULL, 0, &cblk, &cslot);
            pfree(copy);
            if (payload_bytes)
                pfree(payload_bytes);
            *inline_data_out  = NULL;
            *inline_bytes_out = 0;
            *coalesced_out = true;
            *cblk_out = cblk;
            *cslot_out = cslot;
            return InvalidBlockNumber;
        }

        /* Single-leaf case: everything fits in one page */
        if (total <= budget)
        {
            BlockNumber blk = write_single_leaf(b->index, b->trigram_hash,
                                                bytes, sz,
                                                payload_bytes, payload_sz,
                                                b->min_tid, b->max_tid,
                                                b->n_tids,
                                                InvalidBlockNumber);
            pfree(copy);
            if (payload_bytes)
                pfree(payload_bytes);
            *inline_data_out  = NULL;
            *inline_bytes_out = 0;
            return blk;
        }

        /* Multi-leaf case: partition TIDs and write right-to-left chain.
         * Use ~70% of budget per leaf to allow some compression overhead. */
        {
            Size target_leaf_size = (budget * 7) / 10;
            int n_tids = b->n_tids;
            int tid_idx = 0;
            BlockNumber right_link = InvalidBlockNumber;
            BlockNumber leftmost_blk = InvalidBlockNumber;
            Size bloom_bytes = (pg_tre_bloom_tuple_bits + 7) / 8;

            /* Estimate bytes per TID: ~8 for sparsemap RLE (conservative),
             * plus payload overhead if present. */
            Size bytes_per_tid = 8;  /* sparsemap average */
            if (b->with_payload && b->payload_count > 0)
            {
                /* payload: 2 bytes (n_positions) + 4*n_positions + bloom_bytes */
                Size avg_positions = 2;  /* conservative estimate */
                bytes_per_tid += 2 + avg_positions * 4 + bloom_bytes;
            }

            /* Process TIDs right-to-left, building leaves from the end. */
            while (tid_idx < n_tids)
            {
                /* Estimate how many TIDs fit in this leaf */
                int tids_in_leaf = (int)(target_leaf_size / bytes_per_tid);
                if (tids_in_leaf < 1)
                    tids_in_leaf = 1;
                if (tid_idx + tids_in_leaf > n_tids)
                    tids_in_leaf = n_tids - tid_idx;

                /* Build sparsemap for this slice; grow geometrically on
                 * ENOSPC (sparse-TID inputs blow past the 16 B/TID
                 * estimate).  See pg_tre_posting_build_finish for the
                 * full rationale. */
                size_t cap = (size_t)tids_in_leaf * 16 + 1024;
                sm_t *slice_smap = sm_create(cap);
                uint8 *slice_bytes;
                Size slice_sz;
                int k;

                if (slice_smap == NULL)
                    ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
                        errmsg("pg_tre: failed to allocate slice sparsemap")));

                for (k = 0; k < tids_in_leaf; k++)
                {
                    int retries = 0;
                    /* Cancellable; same rationale as the build_finish
                     * loop above. */
                    CHECK_FOR_INTERRUPTS();
                    while (sm_add_grow(&slice_smap, b->tids[tid_idx + k]) == SM_IDX_MAX)
                    {
                        if (++retries > 16)
                        {
                            size_t final_cap = sm_get_capacity(slice_smap);
                            sm_free(slice_smap);
                            ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                                errmsg("pg_tre: multi-leaf slice sm_add_grow exhausted retries (k=%d, tids_in_leaf=%d, cap=%zu)",
                                       k, tids_in_leaf, final_cap)));
                        }
                    }
                }

                slice_sz = sm_get_size(slice_smap);
                slice_bytes = (uint8 *) palloc(slice_sz + 16);
                memcpy(slice_bytes, sm_get_data(slice_smap), slice_sz);
                memset(slice_bytes + slice_sz, 0, 16);
                sm_free(slice_smap);

                /* Build payload slice for this range */
                uint8 *slice_payload = NULL;
                Size slice_payload_sz = 0;
                if (b->with_payload && b->payload_count > 0)
                {
                    /* Serialize just the payload entries for this TID range */
                    slice_payload = serialize_payload(b->payload + tid_idx,
                                                     tids_in_leaf,
                                                     &slice_payload_sz);
                }

                /* Verify this leaf fits within budget */
                if (slice_sz + slice_payload_sz > budget)
                {
                    /* Too big; try with half the TIDs */
                    if (tids_in_leaf <= 1)
                        ereport(ERROR,
                                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                                 errmsg("pg_tre: single TID exceeds leaf budget")));
                    pfree(slice_bytes);
                    if (slice_payload)
                        pfree(slice_payload);
                    /* Retry with half */
                    tids_in_leaf /= 2;
                    continue;
                }

                /* Write this leaf */
                uint64 leaf_min_tid = b->tids[tid_idx];
                uint64 leaf_max_tid = b->tids[tid_idx + tids_in_leaf - 1];
                BlockNumber leaf_blk = write_single_leaf(
                    b->index, b->trigram_hash,
                    slice_bytes, slice_sz,
                    slice_payload, slice_payload_sz,
                    leaf_min_tid, leaf_max_tid,
                    tids_in_leaf,
                    right_link);

                pfree(slice_bytes);
                if (slice_payload)
                    pfree(slice_payload);

                /* This leaf becomes the new leftmost */
                leftmost_blk = leaf_blk;
                right_link = leaf_blk;
                tid_idx += tids_in_leaf;
            }

            /* Cleanup original buffers */
            pfree(copy);
            if (payload_bytes)
                pfree(payload_bytes);
            *inline_data_out  = NULL;
            *inline_bytes_out = 0;
            return leftmost_blk;
        }
    }
}

void
pg_tre_posting_build_free(PgTrePostingBuilder *b)
{
    if (b == NULL)
        return;
    if (b->tids != NULL)
        pfree(b->tids);

    /* Free payload entries (Phase 5). */
    if (b->payload != NULL)
    {
        int i;
        for (i = 0; i < b->payload_count; i++)
        {
            if (b->payload[i].positions != NULL)
                pfree(b->payload[i].positions);
            if (b->payload[i].bloom_bits != NULL)
                pfree(b->payload[i].bloom_bits);
        }
        pfree(b->payload);
    }

    pfree(b);
}

int
pg_tre_posting_build_n_tids(const PgTrePostingBuilder *b)
{
    if (b == NULL)
        return 0;
    return b->n_tids;
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
    sm_t *smap;
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
pg_tre_posting_scan_next(PgTrePostingScan *s, sm_t **out,
                         BlockNumber *min_tid_blk, BlockNumber *max_tid_blk)
{
    scan_release_current(s);

    /* Inline case: serve exactly once. */
    if (s->inline_data != NULL && !s->served_inline)
    {
        /* Wrap over a palloc'd copy so we own stable storage.
         * Call sm_open so m_data_used reflects the serialized
         * content (wrap alone leaves m_data_used=0). */
        uint8 *buf = (uint8 *) palloc(s->inline_bytes);
        memcpy(buf, s->inline_data, s->inline_bytes);
        s->smap = sm_wrap(buf, s->inline_bytes);
        if (s->smap != NULL)
            sm_open(s->smap, buf, s->inline_bytes);
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

        if (!posting_leaf_header_valid(hdr))
        {
            UnlockReleaseBuffer(buf);
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("pg_tre: corrupt posting leaf at block %u",
                            s->cur_blk)));
        }

        /* Copy bytes out so the caller's sparsemap survives unlock. */
        copy = (uint8 *) palloc(hdr->sparsemap_bytes + 8);
        memcpy(copy, sm_bytes, hdr->sparsemap_bytes);

        s->smap = sm_wrap(copy, hdr->sparsemap_bytes);
        if (s->smap != NULL)
            sm_open(s->smap, copy, hdr->sparsemap_bytes);
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

/*
 * Total cardinality (distinct TID count) of a posting tree, for
 * planner selectivity estimation (Phase A / A3).  Walks the
 * right-link leaf chain (or the inline blob) summing each leaf's
 * sparsemap cardinality.  To bound plan-time I/O on pathologically
 * common trigrams, stops once `cap` is exceeded and returns `cap`
 * (the caller only needs "this trigram is common" past that point).
 * A cap <= 0 means no cap.
 */
uint64
pg_tre_posting_cardinality(Relation index, BlockNumber root,
                           const uint8 *inline_data, Size inline_bytes,
                           uint64 cap)
{
    PgTrePostingScan *s;
    sm_t   *sm;
    uint64  total = 0;

    s = pg_tre_posting_scan_begin(index, root, inline_data, inline_bytes);
    while (pg_tre_posting_scan_next(s, &sm, NULL, NULL))
    {
        if (sm != NULL)
            total += (uint64) sm_cardinality(sm);
        if (cap > 0 && total >= cap)
        {
            total = cap;
            break;
        }
    }
    pg_tre_posting_scan_end(s);
    return total;
}

sm_t *
pg_tre_posting_materialize(Relation index, BlockNumber root,
                           const uint8 *inline_data, Size inline_bytes)
{
    /*
     * Return an OWNED sparsemap (allocated by sm_create()) so the
     * caller can safely pass it to sm_union, which may need
     * to grow via sm_set_data_size.  The earlier wrap-based
     * implementation looked correct in isolation but caused heap
     * corruption when the consumer grew the wrap'd map: the wrap'd
     * struct's m_data points at a foreign buffer, so
     * sm_set_data_size cannot in-place realloc and instead
     * silently bumps m_capacity, after which sm_add writes
     * past the actual buffer.  Caught by tap/concurrency.pl.
     */
    if (inline_data != NULL)
    {
        sm_t *sm = sm_open_copy(inline_data, inline_bytes, 64);
        if (sm == NULL)
            return NULL;
        return sm;
    }

    if (BlockNumberIsValid(root))
    {
        Buffer  buf = pg_tre_read(index, root, PG_TRE_PAGE_POSTING_L,
                                  BUFFER_LOCK_SHARE);
        Page    page = BufferGetPage(buf);
        PgTrePostingLeafHeader *hdr =
            (PgTrePostingLeafHeader *) PageGetContents(page);
        uint8  *sm_bytes = (uint8 *) hdr + MAXALIGN(sizeof(*hdr));
        Size    bytes;
        sm_t *sm;

        if (!posting_leaf_header_valid(hdr))
        {
            UnlockReleaseBuffer(buf);
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("pg_tre: corrupt posting leaf at block %u",
                            root)));
        }

        bytes = hdr->sparsemap_bytes;
        sm = sm_open_copy(sm_bytes, bytes, 64);

        if (sm == NULL)
        {
            UnlockReleaseBuffer(buf);
            return NULL;
        }

        /* Phase 4.2: traverse right-link chain and union all leaves. */
        BlockNumber next_blk = hdr->right_link;
        UnlockReleaseBuffer(buf);

        while (BlockNumberIsValid(next_blk))
        {
            Buffer next_buf = pg_tre_read(index, next_blk, PG_TRE_PAGE_POSTING_L,
                                         BUFFER_LOCK_SHARE);
            Page next_page = BufferGetPage(next_buf);
            PgTrePostingLeafHeader *next_hdr =
                (PgTrePostingLeafHeader *) PageGetContents(next_page);
            uint8 *next_sm_bytes = (uint8 *) next_hdr + MAXALIGN(sizeof(*next_hdr));
            Size next_bytes;
            sm_t *next_sm;

            if (!posting_leaf_header_valid(next_hdr))
            {
                UnlockReleaseBuffer(next_buf);
                sm_free(sm);
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("pg_tre: corrupt posting leaf at block %u",
                                next_blk)));
            }

            next_bytes = next_hdr->sparsemap_bytes;

            /* Materialize this leaf's sparsemap. */
            next_sm = sm_open_copy(next_sm_bytes, next_bytes, 64);
            if (next_sm == NULL)
            {
                UnlockReleaseBuffer(next_buf);
                sm_free(sm);
                return NULL;
            }

            /*
             * Union next_sm into sm.  sm_union() returns a freshly
             * allocated result map and does NOT modify either input;
             * NULL means allocation failure.  Earlier code here
             * inverted the success/failure check (treating non-NULL
             * as failure), which silently dropped every leaf past
             * the first and made multi-leaf posting trees return
             * zero rows on lookup.
             */
            sm_t *merged = sm_union(sm, next_sm);
            sm_free(next_sm);
            if (merged == NULL)
            {
                UnlockReleaseBuffer(next_buf);
                sm_free(sm);
                return NULL;
            }
            sm_free(sm);
            sm = merged;

            next_blk = next_hdr->right_link;
            UnlockReleaseBuffer(next_buf);
        }

        return sm;
    }

    /* Empty posting: a 64-byte zeroed sparsemap is a valid empty map. */
    return sm_create(64);
}

/*
 * Lookup the per-tuple bloom filter for a specific TID.
 *
 * Phase 5 WRITE-side implementation.  READ side will extend this with
 * actual filtering logic.  For now, we just provide the extraction
 * mechanism.
 *
 * Returns true if the TID is present in the posting and bloom data is
 * available; false otherwise.  If true, copies the bloom bits into
 * out_bloom (caller must provide a buffer of at least out_bloom_sz bytes,
 * typically (pg_tre_bloom_tuple_bits + 7) / 8).
 */
bool
pg_tre_posting_lookup_tuple_bloom(Relation index,
                                  BlockNumber root,
                                  const uint8 *inline_data,
                                  Size inline_bytes,
                                  uint64 packed_tid,
                                  uint8 *out_bloom,
                                  Size out_bloom_sz,
                                  uint32 *out_page_format_version)
{
    Size bloom_bytes = (pg_tre_bloom_tuple_bits + 7) / 8;
    sm_t *smap = NULL;
    size_t rank;
    const uint8 *payload_base;
    const uint8 *entry_ptr;

    Assert(out_bloom_sz >= bloom_bytes);

    if (out_page_format_version != NULL)
        *out_page_format_version = PG_TRE_FORMAT_VERSION_LATEST;

    /* Step 1: determine if TID is present in the sparsemap. */
    if (inline_data != NULL)
    {
        /* Inline case: sparsemap + payload concatenated. */
        /* Phase 5: assume no payload in inline for now (too complex for
         * first iteration).  Return false. */
        return false;
    }

    if (!BlockNumberIsValid(root))
        return false;

    /* On-disk case: walk right-link chain to find leaf containing packed_tid. */
    {
        BlockNumber cur_blk = root;
        Buffer buf = InvalidBuffer;
        Page page;
        PgTrePostingLeafHeader *hdr;
        uint8 *sm_bytes;
        bool found = false;

        /* Walk right-links until we find the leaf with min_tid <= target <= max_tid */
        while (BlockNumberIsValid(cur_blk))
        {
            buf = pg_tre_read(index, cur_blk, PG_TRE_PAGE_POSTING_L,
                             BUFFER_LOCK_SHARE);
            page = BufferGetPage(buf);
            hdr = (PgTrePostingLeafHeader *) PageGetContents(page);

            /* Check if this leaf contains the target TID */
            if (packed_tid >= hdr->min_tid && packed_tid <= hdr->max_tid)
            {
                /* Found the right leaf */
                break;
            }

            /* Move to next leaf */
            BlockNumber next_blk = hdr->right_link;
            UnlockReleaseBuffer(buf);
            cur_blk = next_blk;

            if (!BlockNumberIsValid(cur_blk))
            {
                /* TID not in any leaf */
                return false;
            }
        }

        if (!BlockNumberIsValid(cur_blk))
            return false;

        /* Now buf points to the leaf containing packed_tid */
        if (!posting_leaf_header_valid(hdr))
        {
            UnlockReleaseBuffer(buf);
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("pg_tre: corrupt posting leaf at block %u",
                            cur_blk)));
        }
        sm_bytes = (uint8 *) hdr + MAXALIGN(sizeof(*hdr));

        /* Check if we have payload data. */
        if (hdr->payload_bytes == 0)
        {
            UnlockReleaseBuffer(buf);
            return false;
        }

        /* Wrap the sparsemap to test membership and compute rank. */
        smap = sm_wrap(sm_bytes, hdr->sparsemap_bytes);
        if (smap != NULL)
            sm_open(smap, sm_bytes, hdr->sparsemap_bytes);
        if (!sm_contains(smap, packed_tid))
        {
            free(smap);
            UnlockReleaseBuffer(buf);
            return false;
        }

        /* Compute rank: sm_rank(x, y, true) returns the count of set
         * bits from x to y INCLUSIVE.  So if packed_tid is the Nth TID
         * (0-indexed), rank will be N+1, and we need to skip N entries.
         * Hence: skip (rank - 1) entries, not rank entries. */
        rank = sm_rank(smap, 0, packed_tid, true);
        free(smap);

        /* Phase 5: payload layout:
         * Each entry is: uint16 n_positions + uint32 positions[n_positions]
         * + uint8 bloom[bloom_bytes].
         * Walk forward 'rank-1' entries to find the target. */
        payload_base = (const uint8 *) page + hdr->payload_offset;
        entry_ptr = payload_base;
        {
            const uint8 *payload_end = payload_base + hdr->payload_bytes;

            for (size_t i = 0; i + 1 < rank; i++)
            {
                uint16 n_pos;
                Size   step;

                if (entry_ptr + sizeof(uint16) > payload_end)
                    goto corrupt_payload;
                memcpy(&n_pos, entry_ptr, sizeof(uint16));
                step = sizeof(uint16)
                     + (Size) n_pos * sizeof(uint32)
                     + bloom_bytes;
                if (step > (Size) (payload_end - entry_ptr))
                    goto corrupt_payload;
                entry_ptr += step;
            }

            /* Now entry_ptr points at the start of the target entry. */
            {
                uint16 n_pos;
                Size   skip;

                if (entry_ptr + sizeof(uint16) > payload_end)
                    goto corrupt_payload;
                memcpy(&n_pos, entry_ptr, sizeof(uint16));
                skip = sizeof(uint16) + (Size) n_pos * sizeof(uint32);
                if (skip > (Size) (payload_end - entry_ptr)
                    || bloom_bytes > (Size) (payload_end - entry_ptr) - skip)
                    goto corrupt_payload;
                entry_ptr += skip;

                /* entry_ptr now points at the bloom bits. */
                memcpy(out_bloom, entry_ptr, bloom_bytes);
                found = true;
            }
        }

        /*
         * Surface the leaf page's per-page format_version so that the
         * caller's bloom decoder can dispatch on it (see the format-
         * version-aware decode helper in src/util/bloom.c).
         */
        if (out_page_format_version != NULL)
            *out_page_format_version = PageTreGetOpaque(page)->format_version;

        UnlockReleaseBuffer(buf);
        return found;

corrupt_payload:
        UnlockReleaseBuffer(buf);
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("pg_tre: corrupt posting payload at block %u",
                        cur_blk)));
        return false;           /* unreachable */
    }
}

/*
 * Look up positions where a trigram appears in a TID.
 * Returns the number of positions found (0 if TID not present).
 *
 * On success *out_positions is set to a buffer palloc'd in
 * CurrentMemoryContext holding the returned positions.  Ownership is
 * the caller's: it is freed when the surrounding memory context is
 * reset/deleted, or the caller may pfree it explicitly.  Each call
 * allocates a fresh buffer, so the result of one call is NOT clobbered
 * by a subsequent (possibly re-entrant) call -- unlike the previous
 * implementation, which returned a pointer into a single process-global
 * static array that aliased across re-entrant scans (issue H4).
 *
 * Phase 5.1: Implements position lookup from payload area.
 */
int
pg_tre_posting_lookup_positions(Relation index,
                                BlockNumber root,
                                const uint8 *inline_data,
                                Size inline_bytes,
                                uint64 packed_tid,
                                const uint32 **out_positions)
{
    /*
     * H4 fix: positions are copied into a buffer palloc'd in
     * CurrentMemoryContext (allocated lazily once we know we have a
     * non-empty position list).  No shared static storage, so
     * concurrent / re-entrant lookups do not alias each other.
     */
    uint32 *positions_buf = NULL;
    Size bloom_bytes = (pg_tre_bloom_tuple_bits + 7) / 8;
    sm_t *smap = NULL;
    size_t rank;
    const uint8 *payload_base;
    const uint8 *entry_ptr;
    uint16 n_positions;
    const uint32 *positions_ptr;

    /* Step 1: determine if TID is present in the sparsemap. */
    if (inline_data != NULL)
    {
        /* Inline case: Phase 5 assumes no payload in inline for now */
        return 0;
    }

    if (!BlockNumberIsValid(root))
        return 0;  /* no posting tree */

    /* Step 2: walk right-link chain to find leaf containing packed_tid */
    {
        BlockNumber cur_blk = root;
        Buffer buf = InvalidBuffer;
        Page page;
        PgTrePostingLeafHeader *hdr;
        Size smap_size;

        /* Walk right-links until we find the leaf with min_tid <= target <= max_tid */
        while (BlockNumberIsValid(cur_blk))
        {
            buf = ReadBuffer(index, cur_blk);
            LockBuffer(buf, BUFFER_LOCK_SHARE);
            page = BufferGetPage(buf);
            hdr = (PgTrePostingLeafHeader *) PageGetContents(page);

            /* Check if this leaf contains the target TID */
            if (packed_tid >= hdr->min_tid && packed_tid <= hdr->max_tid)
            {
                /* Found the right leaf */
                break;
            }

            /* Move to next leaf */
            BlockNumber next_blk = hdr->right_link;
            UnlockReleaseBuffer(buf);
            cur_blk = next_blk;

            if (!BlockNumberIsValid(cur_blk))
            {
                /* TID not in any leaf */
                return 0;
            }
        }

        if (!BlockNumberIsValid(cur_blk))
            return 0;

        /* Now buf points to the leaf containing packed_tid */
        if (!posting_leaf_header_valid(hdr))
        {
            UnlockReleaseBuffer(buf);
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("pg_tre: corrupt posting leaf at block %u",
                            cur_blk)));
        }

        /* Check if this leaf has payload */
        if (hdr->payload_offset == 0)
        {
            UnlockReleaseBuffer(buf);
            return 0;  /* no payload */
        }

        /* Wrap sparsemap.  posting_leaf_header_valid() guarantees
         * payload_offset >= the (MAXALIGN'd) header end, so this
         * subtraction cannot underflow. */
        smap_size = hdr->payload_offset - sizeof(PgTrePostingLeafHeader);
        smap = sm_wrap((uint8 *) (hdr + 1), smap_size);
        if (smap != NULL)
            sm_open(smap, (uint8 *) (hdr + 1), smap_size);

        /* Check if TID is present */
        if (!sm_contains(smap, packed_tid))
        {
            UnlockReleaseBuffer(buf);
            return 0;  /* TID not in posting */
        }

        /* Get rank (entry index) for this TID */
        rank = sm_rank(smap, 0, packed_tid, true);

        /* Payload starts at payload_offset from page start */
        payload_base = (const uint8 *) page + hdr->payload_offset;

        /* Walk payload entries to find our rank, bounds-checking every
         * step against the declared payload region. */
        entry_ptr = payload_base;
        {
            const uint8 *payload_end = payload_base + hdr->payload_bytes;
            size_t entry_idx;
            Size   avail;

            /* Same fix as in lookup_tuple_bloom: skip (rank-1) entries */
            for (entry_idx = 0; entry_idx + 1 < rank; entry_idx++)
            {
                uint16 entry_n_positions;
                Size   step;

                if (entry_ptr + sizeof(uint16) > payload_end)
                    goto corrupt_positions;
                memcpy(&entry_n_positions, entry_ptr, sizeof(uint16));
                step = sizeof(uint16)
                     + (Size) entry_n_positions * sizeof(uint32)
                     + bloom_bytes;
                if (step > (Size) (payload_end - entry_ptr))
                    goto corrupt_positions;
                entry_ptr += step;
            }

            /* Now entry_ptr points at our TID's payload entry */
            if (entry_ptr + sizeof(uint16) > payload_end)
                goto corrupt_positions;
            memcpy(&n_positions, entry_ptr, sizeof(uint16));
            entry_ptr += sizeof(uint16);

            if (n_positions > 1024)
                n_positions = 1024;  /* cap at buffer size */

            /* Clamp to what actually remains in the payload region. */
            avail = (Size) (payload_end - entry_ptr) / sizeof(uint32);
            if ((Size) n_positions > avail)
                n_positions = (uint16) avail;

            positions_ptr = (const uint32 *) entry_ptr;

            /*
             * Copy positions into a per-call buffer palloc'd in
             * CurrentMemoryContext (H4: no shared static storage).
             * When n_positions is 0 we still hand back a valid (1-elem)
             * buffer so *out_positions is never left dangling.
             */
            positions_buf = (uint32 *) palloc((n_positions > 0
                                               ? (Size) n_positions : 1)
                                              * sizeof(uint32));
            if (n_positions > 0)
                memcpy(positions_buf, positions_ptr,
                       (Size) n_positions * sizeof(uint32));
            *out_positions = positions_buf;

            UnlockReleaseBuffer(buf);
            return (int) n_positions;
        }

corrupt_positions:
        UnlockReleaseBuffer(buf);
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("pg_tre: corrupt posting payload at block %u",
                        cur_blk)));
        return 0;               /* unreachable */
    }
}

/* ================================================================
 * Bulk delete (VACUUM)
 * ================================================================
 *
 * Local mirror of the upper-tree internal-page entry layout.  The
 * canonical definition lives in src/pages/upper.c (not exported via a
 * header); we replicate the on-disk struct here so the vacuum walk can
 * descend internal upper-tree levels.  Keep in sync with upper.c.
 */
typedef struct PgTreVacUpperInternalEntry
{
    uint64      first_key;
    BlockNumber child_blk;
} PgTreVacUpperInternalEntry;

/*
 * C2: remove dead heap TIDs from posting trees.  ambulkdelete()
 * (src/am/amvacuum.c) drives this: for every TID stored in every
 * reachable posting tree we invoke the IndexBulkDeleteCallback; TIDs
 * the callback reports dead are stripped from the leaf sparsemap and
 * their parallel payload entries are dropped.  Surviving TIDs (and
 * their payload) are repacked in place and the leaf is WAL-logged as a
 * full-page image (XLOG_PTRE_VACUUM, which routes through the generic
 * FPI redo path in src/wal/xlog.c).
 *
 * Enumeration: posting roots live in the upper-tree leaf entries
 * (PgTreUpperLeafEntry.posting_root).  We walk the upper tree from
 * meta.root_upper -- descending internal levels to the leftmost leaf is
 * not enough because upper leaves are NOT right-linked (they are
 * bulk-loaded and addressed only via internal pages), so we perform a
 * full recursive descent collecting every leaf's posting roots, then
 * delete from each posting tree (following its right-link chain).
 *
 * Residual gap (documented for the orchestrator): postings stored
 * INLINE in the upper-tree leaf entry (posting_root ==
 * InvalidBlockNumber, inline_bytes > 0) ARE now cleaned as of 1.5.4:
 * the upper-tree leaf page is taken EXCLUSIVE, every inline blob is
 * repacked (dead TIDs stripped from its sparsemap and parallel payload),
 * the entry array's inline_bytes fields are refreshed, the whole inline
 * region is rewritten in place (it only ever shrinks), and the page is
 * WAL-logged as a full-page image (XLOG_PTRE_VACUUM).
 */

/*
 * Repack a single inline posting blob, dropping dead TIDs.  The blob is
 * laid out exactly like pg_tre_posting_build_finish() produces it:
 *   [ serialized sparsemap ][ optional payload stream ]
 * where the sparsemap's own serialized length is self-describing (so we
 * split the payload off after sm_open_copy).  Payload entries are in
 * sparsemap rank order, identical to an on-disk leaf.
 *
 * Writes the repacked blob into out_buf (which must be at least
 * in_bytes large -- output never exceeds input) and returns its length
 * via *out_len.  Returns the number of TIDs removed; *out_remaining
 * receives the surviving count.
 */
static uint64
repack_inline_posting(const uint8 *in, Size in_bytes,
                      uint8 *out_buf, Size *out_len,
                      IndexBulkDeleteCallback callback, void *callback_state,
                      uint64 *out_remaining)
{
    Size    bloom_bytes = (pg_tre_bloom_tuple_bits + 7) / 8;
    sm_t *smap;
    Size    sm_size;
    bool    has_payload;
    const uint8 *payload_base = NULL;
    const uint8 *payload_end = NULL;
    const uint8 *entry_ptr = NULL;

    uint64 *keep_tids = NULL;
    int     keep_n = 0;
    int     keep_cap = 0;
    uint64  removed = 0;
    uint64  member;

    uint8  *new_payload = NULL;
    Size    new_payload_used = 0;

    /* Open a copy so we can read members; sm_get_size() on the copy
     * yields the true serialized sparsemap prefix length. */
    smap = sm_open_copy(in, in_bytes, 64);
    if (smap == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("pg_tre: corrupt inline posting (sparsemap open failed)")));
    sm_size = sm_get_size(smap);
    if (sm_size > in_bytes)
    {
        sm_free(smap);
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("pg_tre: corrupt inline posting (sparsemap > blob)")));
    }

    has_payload = (in_bytes > sm_size);
    if (has_payload)
    {
        payload_base = in + sm_size;
        payload_end  = in + in_bytes;
        entry_ptr    = payload_base;
        new_payload  = (uint8 *) palloc(in_bytes - sm_size);
    }

    member = SM_IDX_MAX;
    while ((member = sm_next_member(smap, member)) != SM_IDX_MAX)
    {
        ItemPointerData iptr;
        bool    dead;
        const uint8 *this_entry = NULL;
        Size    this_entry_len = 0;

        CHECK_FOR_INTERRUPTS();

        if (has_payload)
        {
            uint16 n_pos;
            Size   step;

            if (entry_ptr + sizeof(uint16) > payload_end)
                goto corrupt;
            memcpy(&n_pos, entry_ptr, sizeof(uint16));
            step = sizeof(uint16) + (Size) n_pos * sizeof(uint32) + bloom_bytes;
            if (step > (Size) (payload_end - entry_ptr))
                goto corrupt;
            this_entry = entry_ptr;
            this_entry_len = step;
            entry_ptr += step;
        }

        pg_tre_unpack_tid(member, &iptr);
        dead = callback(&iptr, callback_state);

        if (dead)
        {
            removed++;
            continue;
        }

        if (keep_n >= keep_cap)
        {
            keep_cap = keep_cap ? keep_cap * 2 : 64;
            keep_tids = (uint64 *) (keep_tids
                ? repalloc(keep_tids, (Size) keep_cap * sizeof(uint64))
                : palloc((Size) keep_cap * sizeof(uint64)));
        }
        keep_tids[keep_n++] = member;

        if (has_payload)
        {
            memcpy(new_payload + new_payload_used, this_entry, this_entry_len);
            new_payload_used += this_entry_len;
        }
    }

    sm_free(smap);

    if (out_remaining)
        *out_remaining = (uint64) keep_n;

    /* Nothing dead: hand back the original blob verbatim. */
    if (removed == 0)
    {
        memcpy(out_buf, in, in_bytes);
        *out_len = in_bytes;
        if (keep_tids)
            pfree(keep_tids);
        if (new_payload)
            pfree(new_payload);
        return 0;
    }

    /* Rebuild the sparsemap from survivors. */
    {
        sm_t *fresh;
        Size    new_sm_size;
        int     k;

        fresh = sm_create((size_t) keep_n * 16 + 1024);
        if (fresh == NULL)
            ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
                errmsg("pg_tre: failed to allocate sparsemap for inline vacuum repack")));
        for (k = 0; k < keep_n; k++)
        {
            int retries = 0;
            CHECK_FOR_INTERRUPTS();
            while (sm_add_grow(&fresh, keep_tids[k]) == SM_IDX_MAX)
            {
                if (++retries > 16)
                {
                    sm_free(fresh);
                    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                        errmsg("pg_tre: inline vacuum repack sm_add_grow exhausted retries")));
                }
            }
        }
        new_sm_size = sm_get_size(fresh);

        if (new_sm_size + new_payload_used > in_bytes)
        {
            sm_free(fresh);
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("pg_tre: inline vacuum repack overflow")));
        }

        memcpy(out_buf, sm_get_data(fresh), new_sm_size);
        if (new_payload_used > 0)
            memcpy(out_buf + new_sm_size, new_payload, new_payload_used);
        *out_len = new_sm_size + new_payload_used;
        sm_free(fresh);
    }

    if (keep_tids)
        pfree(keep_tids);
    if (new_payload)
        pfree(new_payload);
    return removed;

corrupt:
    sm_free(smap);
    if (keep_tids)
        pfree(keep_tids);
    if (new_payload)
        pfree(new_payload);
    ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("pg_tre: corrupt inline posting payload")));
    return 0;                   /* unreachable */
}

/*
 * Repack all inline postings on one upper-tree leaf page, dropping dead
 * TIDs.  The caller holds an EXCLUSIVE lock on buf.  Rewrites the inline
 * region (concatenated blobs after the entry array) in place -- output
 * is always <= input since we only remove -- refreshes each entry's
 * inline_bytes, fixes pd_lower, WAL-logs the page, and leaves the lock
 * held (caller releases).  Returns the number of TIDs removed across all
 * inline entries; accumulates surviving inline TIDs into *out_remaining.
 */
static uint64
posting_leaf_inline_delete(Relation index, Buffer buf, BlockNumber blkno,
                           IndexBulkDeleteCallback callback,
                           void *callback_state, uint64 *out_remaining)
{
    Page    page = BufferGetPage(buf);
    PageTreOpaque opq = PageTreGetOpaque(page);
    PgTreUpperLeafEntry *entries =
        (PgTreUpperLeafEntry *) PageGetContents(page);
    int     n_entries = opq->flags;
    uint8  *inline_region;
    Size    old_region_used = 0;
    Size    new_region_used = 0;
    uint8  *new_region = NULL;
    uint64  removed = 0;
    int     i;
    bool    any_inline = false;

    if (n_entries == 0)
        n_entries = (((PageHeader) page)->pd_lower - sizeof(PageHeaderData))
                  / sizeof(PgTreUpperLeafEntry);
    if (n_entries <= 0)
        return 0;

    for (i = 0; i < n_entries; i++)
    {
        /*
         * A coalesced entry (PG_TRE_COALESCED_FLAG set in inline_bytes)
         * carries a (flag | slot) marker, NOT a blob length -- it has
         * NO inline blob in this leaf.  Treating its marker as a byte
         * count would read/write far past the page.  Skip it here and
         * preserve it verbatim in the repack loop below.
         */
        if ((entries[i].inline_bytes & PG_TRE_COALESCED_FLAG) != 0)
            continue;
        if (entries[i].inline_bytes > 0)
        {
            any_inline = true;
            old_region_used += entries[i].inline_bytes;
        }
    }
    if (!any_inline)
        return 0;

    /* Inline region begins immediately after the entry array. */
    inline_region = (uint8 *) &entries[n_entries];

    /*
     * Bounds-check the region against pd_lower before touching it.  The
     * entry array + inline region together end at pd_lower.
     */
    {
        Size content_off = (char *) inline_region - (char *) page;
        if (content_off + old_region_used > (Size) ((PageHeader) page)->pd_lower)
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("pg_tre: corrupt upper leaf inline region at block %u",
                            blkno)));
    }

    /* Repack each blob into a fresh contiguous region (<= old size). */
    new_region = (uint8 *) palloc(old_region_used);
    {
        Size    in_off = 0;
        for (i = 0; i < n_entries; i++)
        {
            Size    blob_in = entries[i].inline_bytes;
            Size    blob_out = 0;
            uint64  blob_remaining = 0;

            /* Coalesced marker: no inline blob to repack; leave the
             * entry (and its flag|slot inline_bytes) untouched. */
            if ((entries[i].inline_bytes & PG_TRE_COALESCED_FLAG) != 0)
                continue;
            if (blob_in == 0)
                continue;

            removed += repack_inline_posting(inline_region + in_off, blob_in,
                                             new_region + new_region_used,
                                             &blob_out, callback,
                                             callback_state, &blob_remaining);
            entries[i].inline_bytes = (uint32) blob_out;
            new_region_used += blob_out;
            in_off += blob_in;
            if (out_remaining)
                *out_remaining += blob_remaining;
        }
    }

    /* Nothing actually removed: drop the rebuilt copy, leave page as-is. */
    if (removed == 0)
    {
        /* entries[].inline_bytes are unchanged on the no-removal path
         * (repack_inline_posting copies the blob verbatim and returns the
         * same length), so the page content is byte-identical. */
        pfree(new_region);
        return 0;
    }

    /* Write the repacked inline region back in place and shrink pd_lower. */
    memcpy(inline_region, new_region, new_region_used);
    ((PageHeader) page)->pd_lower =
        ((char *) inline_region + new_region_used) - (char *) page;
    pfree(new_region);

    MarkBufferDirty(buf);
    if (RelationNeedsWAL(index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_VACUUM);
        PageSetLSN(page, recptr);
    }

    return removed;
}

/*
 * Repack one posting leaf, dropping dead TIDs.  Returns the number of
 * TIDs removed from this leaf (0 if nothing changed).  *out_remaining
 * receives the surviving TID count.  The caller holds an EXCLUSIVE lock
 * on buf; this routine writes back in place, WAL-logs, and leaves the
 * lock held (the caller releases).
 */
static uint64
posting_leaf_delete(Relation index, Buffer buf, BlockNumber blkno,
                    IndexBulkDeleteCallback callback, void *callback_state,
                    uint64 *out_remaining)
{
    Page    page = BufferGetPage(buf);
    PgTrePostingLeafHeader *hdr =
        (PgTrePostingLeafHeader *) PageGetContents(page);
    uint8  *sm_bytes;
    sm_t *smap;
    Size    bloom_bytes = (pg_tre_bloom_tuple_bits + 7) / 8;
    Size    smap_size;

    /* surviving state */
    uint64 *keep_tids = NULL;
    int     keep_n = 0;
    int     keep_cap = 0;
    uint64  new_min = UINT64_MAX;
    uint64  new_max = 0;
    uint64  removed = 0;

    /* surviving payload, rebuilt as a flat byte stream in rank order */
    bool    has_payload;
    const uint8 *payload_base = NULL;
    const uint8 *payload_end = NULL;
    const uint8 *entry_ptr = NULL;
    uint8  *new_payload = NULL;
    Size    new_payload_used = 0;
    Size    new_payload_cap = 0;

    uint64  member;

    if (!posting_leaf_header_valid(hdr))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("pg_tre: corrupt posting leaf at block %u", blkno)));

    sm_bytes = (uint8 *) hdr + MAXALIGN(sizeof(*hdr));
    smap_size = hdr->sparsemap_bytes;

    has_payload = (hdr->payload_bytes > 0 && hdr->payload_offset != 0);
    if (has_payload)
    {
        payload_base = (const uint8 *) page + hdr->payload_offset;
        payload_end  = payload_base + hdr->payload_bytes;
        entry_ptr    = payload_base;
        new_payload_cap = hdr->payload_bytes;
        new_payload = (uint8 *) palloc(new_payload_cap > 0 ? new_payload_cap : 1);
    }

    /*
     * Wrap (read-only) the on-page sparsemap to iterate its members in
     * ascending order.  Payload entries are stored in this same rank
     * order, so we walk both in lockstep.
     */
    smap = sm_wrap(sm_bytes, smap_size);
    if (smap != NULL)
        sm_open(smap, sm_bytes, smap_size);

    member = SM_IDX_MAX;
    while (smap != NULL && (member = sm_next_member(smap, member)) != SM_IDX_MAX)
    {
        ItemPointerData iptr;
        bool    dead;
        const uint8 *this_entry = NULL;
        Size    this_entry_len = 0;

        CHECK_FOR_INTERRUPTS();

        /* Locate this TID's payload entry (variable-length) before we
         * decide to keep or drop, so we can copy it on the keep path. */
        if (has_payload)
        {
            uint16 n_pos;
            Size   step;

            if (entry_ptr + sizeof(uint16) > payload_end)
                goto corrupt;
            memcpy(&n_pos, entry_ptr, sizeof(uint16));
            step = sizeof(uint16)
                 + (Size) n_pos * sizeof(uint32)
                 + bloom_bytes;
            if (step > (Size) (payload_end - entry_ptr))
                goto corrupt;
            this_entry = entry_ptr;
            this_entry_len = step;
            entry_ptr += step;
        }

        pg_tre_unpack_tid(member, &iptr);
        dead = callback(&iptr, callback_state);

        if (dead)
        {
            removed++;
            continue;
        }

        /* Keep this TID. */
        if (keep_n >= keep_cap)
        {
            keep_cap = keep_cap ? keep_cap * 2 : 64;
            keep_tids = (uint64 *) (keep_tids
                ? repalloc(keep_tids, (Size) keep_cap * sizeof(uint64))
                : palloc((Size) keep_cap * sizeof(uint64)));
        }
        keep_tids[keep_n++] = member;
        if (member < new_min) new_min = member;
        if (member > new_max) new_max = member;

        if (has_payload)
        {
            /* Append the preserved entry to the rebuilt payload stream.
             * new_payload_cap == old payload_bytes is always sufficient
             * since we only ever drop entries. */
            Assert(new_payload_used + this_entry_len <= new_payload_cap);
            memcpy(new_payload + new_payload_used, this_entry, this_entry_len);
            new_payload_used += this_entry_len;
        }
    }

    if (smap != NULL)
        free(smap);

    if (out_remaining)
        *out_remaining = (uint64) keep_n;

    /* Nothing dead in this leaf: leave the page untouched. */
    if (removed == 0)
    {
        if (keep_tids)
            pfree(keep_tids);
        if (new_payload)
            pfree(new_payload);
        return 0;
    }

    /*
     * Rebuild the leaf in place.  Serialize the surviving TIDs into a
     * fresh sparsemap, then lay out header / sparsemap / payload exactly
     * as write_single_leaf() does.  The new content is always <= the old
     * content (we only removed entries), so it fits on the same page.
     */
    {
        uint8  *new_sm_bytes;
        Size    new_sm_size;
        char   *sm_area;
        sm_t *fresh;
        int     k;

        if (keep_n > 0)
        {
            size_t cap = (size_t) keep_n * 16 + 1024;
            fresh = sm_create(cap);
            if (fresh == NULL)
                ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
                    errmsg("pg_tre: failed to allocate sparsemap for vacuum repack")));
            for (k = 0; k < keep_n; k++)
            {
                int retries = 0;
                CHECK_FOR_INTERRUPTS();
                while (sm_add_grow(&fresh, keep_tids[k]) == SM_IDX_MAX)
                {
                    if (++retries > 16)
                    {
                        sm_free(fresh);
                        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                            errmsg("pg_tre: vacuum repack sm_add_grow exhausted retries")));
                    }
                }
            }
            new_sm_size = sm_get_size(fresh);
            new_sm_bytes = (uint8 *) palloc(new_sm_size + 16);
            memcpy(new_sm_bytes, sm_get_data(fresh), new_sm_size);
            sm_free(fresh);
        }
        else
        {
            /* Leaf emptied entirely.  Keep a valid empty sparsemap blob;
             * the leaf stays linked in the chain (we do not unlink/free
             * pages in this pass) but matches no TIDs. */
            fresh = sm_create(64);
            if (fresh == NULL)
                ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
                    errmsg("pg_tre: failed to allocate empty sparsemap for vacuum repack")));
            new_sm_size = sm_get_size(fresh);
            new_sm_bytes = (uint8 *) palloc(new_sm_size + 16);
            memcpy(new_sm_bytes, sm_get_data(fresh), new_sm_size);
            sm_free(fresh);
            new_min = 0;
            new_max = 0;
        }

        /* The repacked content must never exceed the original budget. */
        if (new_sm_size + new_payload_used > posting_leaf_budget())
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("pg_tre: vacuum repack overflow on block %u", blkno)));

        /* Header: preserve right_link; refresh min/max/sizes/counts. */
        hdr->min_tid         = new_min;
        hdr->max_tid         = new_max;
        hdr->sparsemap_bytes = (uint32) new_sm_size;
        hdr->n_entries       = (keep_n > UINT16_MAX) ? UINT16_MAX : (uint16) keep_n;
        hdr->_pad            = 0;

        sm_area = (char *) hdr + MAXALIGN(sizeof(*hdr));
        memcpy(sm_area, new_sm_bytes, new_sm_size);
        pfree(new_sm_bytes);

        if (has_payload && new_payload_used > 0)
        {
            char *payload_area = (char *) page + BLCKSZ
                               - MAXALIGN(sizeof(PageTreOpaqueData))
                               - new_payload_used;
            memcpy(payload_area, new_payload, new_payload_used);
            hdr->payload_bytes  = (uint32) new_payload_used;
            hdr->payload_offset = payload_area - (char *) page;
            ((PageHeader) page)->pd_lower = (sm_area + new_sm_size) - (char *) page;
            ((PageHeader) page)->pd_upper = payload_area - (char *) page;
        }
        else
        {
            hdr->payload_bytes  = 0;
            hdr->payload_offset = 0;
            ((PageHeader) page)->pd_lower = (sm_area + new_sm_size) - (char *) page;
            /* Reclaim the former payload region: reset pd_upper to the
             * end of the usable area (start of the opaque trailer) so the
             * whole gap is free space and REGBUF_STANDARD strips it from
             * the FPI. */
            ((PageHeader) page)->pd_upper =
                BLCKSZ - MAXALIGN(sizeof(PageTreOpaqueData));
        }
    }

    if (keep_tids)
        pfree(keep_tids);
    if (new_payload)
        pfree(new_payload);

    /* WAL: MarkBufferDirty BEFORE XLogRegisterBuffer (PG18 asserts the
     * buffer is dirty + exclusively locked); FPI replay handled by
     * pg_tre_redo_fpi via XLOG_PTRE_VACUUM. */
    MarkBufferDirty(buf);
    if (RelationNeedsWAL(index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_VACUUM);
        PageSetLSN(page, recptr);
    }

    return removed;

corrupt:
    if (smap != NULL)
        free(smap);
    if (keep_tids)
        pfree(keep_tids);
    if (new_payload)
        pfree(new_payload);
    ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("pg_tre: corrupt posting payload at block %u", blkno)));
    return 0;                   /* unreachable */
}

/*
 * Delete dead TIDs from one posting tree (the right-link chain rooted at
 * `root`).  Walks the chain with lock coupling: the predecessor leaf is
 * held EXCLUSIVE while the current leaf is locked, repacked and (if it
 * emptied) unlinked.  Coupling is required so an emptied non-head leaf
 * can be spliced out of the chain atomically with respect to other
 * vacuums; concurrent scanners only ever hold one leaf lock at a time so
 * there is no lock-order deadlock.  The chain head is never unlinked (it
 * is referenced from the upper-tree leaf entry); an emptied head simply
 * stays as an empty leaf, as before.
 *
 * Accumulates removed / remaining counts and bumps *pages_visited per
 * leaf seen and *pages_deleted per leaf unlinked (deferred-recycle).
 */
static void
posting_tree_delete(Relation index, BlockNumber root,
                    IndexBulkDeleteCallback callback, void *callback_state,
                    uint64 *tuples_removed, uint64 *tuples_remaining,
                    BlockNumber *pages_visited, BlockNumber *pages_deleted)
{
    BlockNumber cur = root;
    Buffer      prev_buf = InvalidBuffer;   /* predecessor, kept locked */

    while (BlockNumberIsValid(cur))
    {
        Buffer  buf = pg_tre_read(index, cur, PG_TRE_PAGE_POSTING_L,
                                  BUFFER_LOCK_EXCLUSIVE);
        Page    page = BufferGetPage(buf);
        PgTrePostingLeafHeader *hdr =
            (PgTrePostingLeafHeader *) PageGetContents(page);
        BlockNumber next;
        uint64  remaining = 0;
        uint64  removed;
        bool    unlinked = false;

        if (!posting_leaf_header_valid(hdr))
        {
            UnlockReleaseBuffer(buf);
            if (BufferIsValid(prev_buf))
                UnlockReleaseBuffer(prev_buf);
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("pg_tre: corrupt posting leaf at block %u", cur)));
        }
        next = hdr->right_link;

        removed = posting_leaf_delete(index, buf, cur, callback,
                                      callback_state, &remaining);
        if (tuples_removed)
            *tuples_removed += removed;
        if (tuples_remaining)
            *tuples_remaining += remaining;
        if (pages_visited)
            (*pages_visited)++;

        /*
         * If this non-head leaf is now empty, splice it out of the chain
         * and mark it deleted (deferred recycle).  We need the
         * predecessor still locked to retarget its right_link.  The head
         * (prev_buf == Invalid) is never unlinked.
         */
        if (remaining == 0 && BufferIsValid(prev_buf))
        {
            posting_leaf_unlink(index, prev_buf, buf, cur);
            unlinked = true;
            if (pages_deleted)
                (*pages_deleted)++;
        }

        if (unlinked)
        {
            /*
             * prev_buf->right_link now points at `next`; keep prev_buf
             * locked as the predecessor for the next iteration and drop
             * the just-unlinked page.
             */
            UnlockReleaseBuffer(buf);
        }
        else
        {
            /* Advance the coupling window: this leaf becomes prev. */
            if (BufferIsValid(prev_buf))
                UnlockReleaseBuffer(prev_buf);
            prev_buf = buf;
        }
        cur = next;
    }

    if (BufferIsValid(prev_buf))
        UnlockReleaseBuffer(prev_buf);
}

/*
 * Physically reclaim deleted posting leaves into the index FSM.  Called
 * from amvacuumcleanup.  Sweeps every block of the main fork; any
 * POSTING_L page flagged PG_TRE_LEAF_DELETED whose deletion XID is old
 * enough that no snapshot could still reach it via a pre-unlink
 * right_link is re-initialized as a blank page (WAL-logged) and recorded
 * free.  Returns the number of pages recycled into the FSM; *out_pending
 * (optional) receives the count of deleted pages NOT yet recyclable
 * (still within the visibility horizon).
 */
BlockNumber
pg_tre_posting_recycle_deleted(Relation index, Relation heaprel,
                               BlockNumber *out_pending)
{
    BlockNumber nblocks = RelationGetNumberOfBlocks(index);
    BlockNumber blk;
    BlockNumber recycled = 0;
    BlockNumber pending = 0;
    bool        any_recorded = false;

    for (blk = 1; blk < nblocks; blk++)   /* block 0 is meta */
    {
        Buffer  buf;
        Page    page;
        PageTreOpaque opq;

        CHECK_FOR_INTERRUPTS();

        buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        page = BufferGetPage(buf);

        if (PageIsNew(page))
        {
            UnlockReleaseBuffer(buf);
            continue;
        }

        opq = PageTreGetOpaque(page);
        if (opq->page_kind != PG_TRE_PAGE_POSTING_L ||
            !(opq->flags & PG_TRE_LEAF_DELETED))
        {
            UnlockReleaseBuffer(buf);
            continue;
        }

        /*
         * Deleted leaf.  Recyclable only once its deletion XID is below
         * the global removable horizon -- i.e. no snapshot still running
         * could have begun a scan that copied a right_link pointing here
         * before the unlink.  This is exactly nbtree's safexid gate.
         */
        if (!GlobalVisCheckRemovableFullXid(heaprel, posting_leaf_deleted_xid(page)))
        {
            pending++;
            UnlockReleaseBuffer(buf);
            continue;
        }

        /* Re-initialize as a blank page and WAL-log, then record free. */
        pg_tre_page_init(page, BLCKSZ, PG_TRE_PAGE_POSTING_L);
        MarkBufferDirty(buf);
        if (RelationNeedsWAL(index))
        {
            XLogRecPtr recptr;

            XLogBeginInsert();
            XLogRegisterBuffer(0, buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
            recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_POSTING_RECYCLE);
            PageSetLSN(page, recptr);
        }
        UnlockReleaseBuffer(buf);

        RecordFreeIndexPage(index, blk);
        any_recorded = true;
        recycled++;
    }

    if (any_recorded)
        IndexFreeSpaceMapVacuum(index);

    if (out_pending)
        *out_pending = pending;
    return recycled;
}

/*
 * Recursively walk the upper tree collecting posting roots and deleting
 * dead TIDs from each.  `blk` is the current upper-tree page.  Internal
 * pages dispatch to children; leaf pages enumerate their entries.
 *
 * Inline postings (posting_root == InvalidBlockNumber, inline_bytes > 0)
 * are repacked in place on the leaf page itself (see
 * posting_leaf_inline_delete); their surviving TIDs are counted into
 * *tuples_remaining, so num_index_tuples is now exact.
 */
static void
posting_upper_walk(Relation index, BlockNumber blk,
                   IndexBulkDeleteCallback callback, void *callback_state,
                   uint64 *tuples_removed, uint64 *tuples_remaining,
                   BlockNumber *posting_pages, BlockNumber *pages_deleted)
{
    Buffer  buf;
    Page    page;
    PageTreOpaque opq;

    CHECK_FOR_INTERRUPTS();

    /*
     * Lock EXCLUSIVE up front: internal pages downgrade-by-releasing
     * before recursing, but leaf pages may need to rewrite inline blobs
     * in place, so we cannot use a SHARE lock for them.  We do not know
     * the page kind until we read it; taking EXCLUSIVE unconditionally is
     * simplest and the upper tree is tiny relative to posting trees.
     */
    buf = pg_tre_read(index, blk, PG_TRE_PAGE_INVALID, BUFFER_LOCK_EXCLUSIVE);
    page = BufferGetPage(buf);
    opq = PageTreGetOpaque(page);

    if (opq->page_kind == PG_TRE_PAGE_UPPER)
    {
        /* Internal node: collect child block numbers, then recurse after
         * releasing this page's lock (avoid holding locks across the
         * descent / posting-tree exclusive locks). */
        PgTreVacUpperInternalEntry *entries =
            (PgTreVacUpperInternalEntry *) PageGetContents(page);
        int n = (((PageHeader) page)->pd_lower - sizeof(PageHeaderData))
              / sizeof(PgTreVacUpperInternalEntry);
        BlockNumber *children = NULL;
        int i;

        if (n > 0)
        {
            children = (BlockNumber *) palloc((Size) n * sizeof(BlockNumber));
            for (i = 0; i < n; i++)
                children[i] = entries[i].child_blk;
        }
        UnlockReleaseBuffer(buf);

        for (i = 0; i < n; i++)
            posting_upper_walk(index, children[i], callback, callback_state,
                               tuples_removed, tuples_remaining, posting_pages,
                               pages_deleted);
        if (children)
            pfree(children);
        return;
    }

    if (opq->page_kind == PG_TRE_PAGE_UPPER_L)
    {
        /* Leaf node: collect posting roots, repack inline postings in
         * place (while we still hold the exclusive lock), then release
         * before descending into the out-of-line posting trees. */
        PgTreUpperLeafEntry *entries =
            (PgTreUpperLeafEntry *) PageGetContents(page);
        int n = opq->flags;
        BlockNumber *roots = NULL;
        int roots_n = 0;
        uint64 inline_removed;
        int i;

        if (n == 0)
            n = (((PageHeader) page)->pd_lower - sizeof(PageHeaderData))
              / sizeof(PgTreUpperLeafEntry);

        if (n > 0)
        {
            roots = (BlockNumber *) palloc((Size) n * sizeof(BlockNumber));
            for (i = 0; i < n; i++)
            {
                /*
                 * Skip coalesced entries: their posting_root is the
                 * block of a shared PG_TRE_PAGE_POSTING_COALESCED page,
                 * NOT a posting-tree root, so it must not be fed to
                 * posting_tree_delete (which would walk it as a posting
                 * tree -- wrong page kind).  Dead TIDs in a coalesced
                 * posting are not reclaimed here; they are filtered by
                 * the executor recheck (correct, just not space-
                 * reclaimed), and a later merge that touches the
                 * trigram rebuilds it as a dedicated leaf.  Full
                 * coalesced-page reclaim is posting-page-coalescing
                 * Phase 4.
                 */
                if ((entries[i].inline_bytes & PG_TRE_COALESCED_FLAG) != 0)
                    continue;
                /* Out-of-line posting trees are handled after release. */
                if (BlockNumberIsValid(entries[i].posting_root))
                    roots[roots_n++] = entries[i].posting_root;
            }
        }

        /* Repack inline postings in place under the exclusive lock. */
        inline_removed = posting_leaf_inline_delete(index, buf, blk, callback,
                                                    callback_state,
                                                    tuples_remaining);
        if (tuples_removed)
            *tuples_removed += inline_removed;

        UnlockReleaseBuffer(buf);

        for (i = 0; i < roots_n; i++)
            posting_tree_delete(index, roots[i], callback, callback_state,
                                tuples_removed, tuples_remaining,
                                posting_pages, pages_deleted);
        if (roots)
            pfree(roots);
        return;
    }

    /* Unexpected page kind: release and ignore (defensive). */
    UnlockReleaseBuffer(buf);
}

/*
 * Top-level bulk delete entry point used by ambulkdelete().  Walks every
 * posting tree reachable from the upper tree, stripping TIDs the
 * callback reports dead.  Returns the number of TIDs removed; reports
 * the surviving TID count via *out_remaining, the number of posting leaf
 * pages visited via *out_pages, and the number of emptied leaves unlinked
 * from their chains (marked deleted, awaiting recycle) via *out_deleted.
 * All out-params are optional.
 */
uint64
pg_tre_posting_bulk_delete(Relation index,
                           IndexBulkDeleteCallback callback,
                           void *callback_state,
                           uint64 *out_remaining,
                           BlockNumber *out_pages,
                           BlockNumber *out_deleted)
{
    PgTreMetaPageData meta;
    uint64  tuples_removed = 0;
    uint64  tuples_remaining = 0;
    BlockNumber posting_pages = 0;
    BlockNumber pages_deleted = 0;

    pg_tre_meta_read(index, &meta);

    if (BlockNumberIsValid(meta.root_upper))
        posting_upper_walk(index, meta.root_upper, callback, callback_state,
                           &tuples_removed, &tuples_remaining, &posting_pages,
                           &pages_deleted);

    if (out_remaining)
        *out_remaining = tuples_remaining;
    if (out_pages)
        *out_pages = posting_pages;
    if (out_deleted)
        *out_deleted = pages_deleted;
    return tuples_removed;
}
