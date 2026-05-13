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
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lockdefs.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "pg_tre/buffer.h"
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

/* ================================================================
 * Builder
 * ================================================================ */

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
     * resize path (sparsemap_set_data_size with data=NULL) triggers glibc
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
     * avoid the dynamic-resize path through sparsemap_set_data_size(),
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
        b->tids_alloced *= 2;
        b->tids = (uint64 *) repalloc(b->tids,
                      b->tids_alloced * sizeof(uint64));
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
            b->payload_alloced *= 2;
            b->payload = (PayloadEntry *) repalloc(b->payload,
                            b->payload_alloced * sizeof(PayloadEntry));
        }

        pe = &b->payload[b->payload_count++];
        pe->tid = packed;
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
                 errmsg("pg_tre: posting for trigram %016" PRIx64 " "
                        "is %zu bytes (sparsemap %zu + payload %zu), "
                        "exceeds single-leaf budget %zu",
                        trigram_hash, total_sz, sz, payload_sz,
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
        XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT | REGBUF_STANDARD);
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
        estimate += payload[i].n_positions * sizeof(uint32);
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
    const uint8 *bytes;
    uint8      *copy;
    uint8      *payload_bytes = NULL;
    Size        payload_sz = 0;
    Size        sz = 0;

    /*
     * Phase 4.1: convert the collected uint64 array of TIDs to a canonical
     * sparsemap serialization.  We allocate a fresh sparsemap of generous
     * capacity (16 bytes per TID + overhead; sparsemap RLE will compress
     * further), add each TID, then copy the resulting m_data bytes out
     * into a palloc'd buffer.
     *
     * This path avoids sparsemap's dynamic-resize bug entirely by sizing
     * the destination correctly from the start.  We still need the raw
     * serialization (via an m_data struct-offset cast) because sparsemap
     * exposes no direct byte-pointer accessor; the documented struct
     * layout { size_t capacity; size_t used; uint8 *data; } makes index
     * [2] on a uint8** cast reach m_data reliably.
     */
    {
        size_t cap = (size_t) b->n_tids * 16 + 1024;
        sparsemap_t *fresh = sparsemap(cap);
        int k;
        if (fresh == NULL)
            ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
                errmsg("pg_tre: failed to allocate sparsemap for serialization")));
        for (k = 0; k < b->n_tids; k++)
        {
            if (sparsemap_add(fresh, b->tids[k]) == SPARSEMAP_IDX_MAX)
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("pg_tre: sparsemap_add overflow for n_tids=%d cap=%zu",
                           b->n_tids, cap)));
        }
        sz = sparsemap_get_size(fresh);
        {
            uint8_t *smap_data = ((uint8_t **) fresh)[2];  /* m_data */
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
        free(fresh);
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

    /* On-disk case: write both sparsemap and payload to a leaf page. */
    {
        BlockNumber blk = write_single_leaf(b->index, b->trigram_hash,
                                            bytes, sz,
                                            payload_bytes, payload_sz,
                                            b->min_tid, b->max_tid,
                                            b->n_tids,
                                            InvalidBlockNumber);
        /* When stored on-disk, we don't need the palloc'd copies. */
        pfree(copy);
        if (payload_bytes)
            pfree(payload_bytes);
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
        /* Wrap over a palloc'd copy so we own stable storage.
         * Call sparsemap_open so m_data_used reflects the serialized
         * content (wrap alone leaves m_data_used=0). */
        uint8 *buf = (uint8 *) palloc(s->inline_bytes);
        memcpy(buf, s->inline_data, s->inline_bytes);
        s->smap = sparsemap_wrap(buf, s->inline_bytes);
        if (s->smap != NULL)
            sparsemap_open(s->smap, buf, s->inline_bytes);
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
        if (s->smap != NULL)
            sparsemap_open(s->smap, copy, hdr->sparsemap_bytes);
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
     * Return an OWNED sparsemap (allocated by sparsemap()) so the
     * caller can safely pass it to sparsemap_union, which may need
     * to grow via sparsemap_set_data_size.  The earlier wrap-based
     * implementation looked correct in isolation but caused heap
     * corruption when the consumer grew the wrap'd map: the wrap'd
     * struct's m_data points at a foreign buffer, so
     * sparsemap_set_data_size cannot in-place realloc and instead
     * silently bumps m_capacity, after which sparsemap_add writes
     * past the actual buffer.  Caught by tap/concurrency.pl.
     */
    if (inline_data != NULL)
    {
        sparsemap_t *sm = sparsemap(inline_bytes + 64);
        if (sm == NULL)
            return NULL;
        memcpy(sparsemap_get_data(sm), inline_data, inline_bytes);
        sparsemap_open(sm, sparsemap_get_data(sm), inline_bytes + 64);
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
        Size    bytes = hdr->sparsemap_bytes;
        sparsemap_t *sm = sparsemap(bytes + 64);

        if (sm == NULL)
        {
            UnlockReleaseBuffer(buf);
            return NULL;
        }
        memcpy(sparsemap_get_data(sm), sm_bytes, bytes);
        sparsemap_open(sm, sparsemap_get_data(sm), bytes + 64);

        /* Phase 2: no right-link traversal (single-leaf postings). */
        UnlockReleaseBuffer(buf);
        return sm;
    }

    /* Empty posting: a 64-byte zeroed sparsemap is a valid empty map. */
    return sparsemap(64);
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
                                  Size out_bloom_sz)
{
    Size bloom_bytes = (pg_tre_bloom_tuple_bits + 7) / 8;
    sparsemap_t *smap = NULL;
    size_t rank;
    const uint8 *payload_base;
    const uint8 *entry_ptr;

    Assert(out_bloom_sz >= bloom_bytes);

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

    /* On-disk case: read the posting leaf. */
    {
        Buffer buf = pg_tre_read(index, root, PG_TRE_PAGE_POSTING_L,
                                 BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buf);
        PgTrePostingLeafHeader *hdr =
            (PgTrePostingLeafHeader *) PageGetContents(page);
        uint8 *sm_bytes = (uint8 *) hdr + MAXALIGN(sizeof(*hdr));
        bool found = false;

        /* Check if we have payload data. */
        if (hdr->payload_bytes == 0)
        {
            UnlockReleaseBuffer(buf);
            return false;
        }

        /* Wrap the sparsemap to test membership and compute rank. */
        smap = sparsemap_wrap(sm_bytes, hdr->sparsemap_bytes);
        if (smap != NULL)
            sparsemap_open(smap, sm_bytes, hdr->sparsemap_bytes);
        if (!sparsemap_contains(smap, packed_tid))
        {
            free(smap);
            UnlockReleaseBuffer(buf);
            return false;
        }

        /* Compute rank: sparsemap_rank(x, y, true) returns the count of set
         * bits from x to y INCLUSIVE.  So if packed_tid is the Nth TID
         * (0-indexed), rank will be N+1, and we need to skip N entries.
         * Hence: skip (rank - 1) entries, not rank entries. */
        rank = sparsemap_rank(smap, 0, packed_tid, true);
        free(smap);

        /* Phase 5: payload layout:
         * Each entry is: uint16 n_positions + uint32 positions[n_positions]
         * + uint8 bloom[bloom_bytes].
         * Walk forward 'rank-1' entries to find the target. */
        payload_base = (const uint8 *) page + hdr->payload_offset;
        entry_ptr = payload_base;

        for (size_t i = 0; i + 1 < rank; i++)
        {
            uint16 n_pos;
            memcpy(&n_pos, entry_ptr, sizeof(uint16));
            entry_ptr += sizeof(uint16);
            entry_ptr += n_pos * sizeof(uint32);
            entry_ptr += bloom_bytes;
        }

        /* Now entry_ptr points at the start of the target entry. */
        {
            uint16 n_pos;
            memcpy(&n_pos, entry_ptr, sizeof(uint16));
            entry_ptr += sizeof(uint16);
            entry_ptr += n_pos * sizeof(uint32);

            /* entry_ptr now points at the bloom bits. */
            memcpy(out_bloom, entry_ptr, bloom_bytes);
            found = true;
        }

        UnlockReleaseBuffer(buf);
        return found;
    }
}

/*
 * Look up positions where a trigram appears in a TID.
 * Returns the number of positions found (0 if TID not present).
 * The returned positions array points into internal storage.
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
    static uint32 positions_buf[1024];  /* thread-local buffer */
    Size bloom_bytes = (pg_tre_bloom_tuple_bits + 7) / 8;
    sparsemap_t *smap = NULL;
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

    /* Step 2: read posting leaf page */
    {
        Buffer buf;
        Page page;
        PgTrePostingLeafHeader *hdr;
        Size smap_size;

        buf = ReadBuffer(index, root);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);
        hdr = (PgTrePostingLeafHeader *) PageGetContents(page);

        /* Check if this leaf has payload */
        if (hdr->payload_offset == 0)
        {
            UnlockReleaseBuffer(buf);
            return 0;  /* no payload */
        }

        /* Wrap sparsemap */
        smap_size = hdr->payload_offset - sizeof(PgTrePostingLeafHeader);
        smap = sparsemap_wrap((uint8 *) (hdr + 1), smap_size);
        if (smap != NULL)
            sparsemap_open(smap, (uint8 *) (hdr + 1), smap_size);

        /* Check if TID is present */
        if (!sparsemap_contains(smap, packed_tid))
        {
            UnlockReleaseBuffer(buf);
            return 0;  /* TID not in posting */
        }

        /* Get rank (entry index) for this TID */
        rank = sparsemap_rank(smap, 0, packed_tid, true);

        /* Payload starts at payload_offset from page start */
        payload_base = (const uint8 *) page + hdr->payload_offset;

        /* Walk payload entries to find our rank */
        entry_ptr = payload_base;
        {
            size_t entry_idx;
            /* Same fix as in lookup_tuple_bloom: skip (rank-1) entries */
            for (entry_idx = 0; entry_idx + 1 < rank; entry_idx++)
            {
                uint16 entry_n_positions;
                memcpy(&entry_n_positions, entry_ptr, sizeof(uint16));
                entry_ptr += sizeof(uint16);
                entry_ptr += entry_n_positions * sizeof(uint32);
                entry_ptr += bloom_bytes;
            }
        }

        /* Now entry_ptr points at our TID's payload entry */
        memcpy(&n_positions, entry_ptr, sizeof(uint16));
        entry_ptr += sizeof(uint16);

        if (n_positions > 1024)
            n_positions = 1024;  /* cap at buffer size */

        positions_ptr = (const uint32 *) entry_ptr;

        /* Copy positions to thread-local buffer */
        memcpy(positions_buf, positions_ptr, n_positions * sizeof(uint32));
        *out_positions = positions_buf;

        UnlockReleaseBuffer(buf);
        return (int) n_positions;
    }
}
