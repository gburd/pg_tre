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
#include "pg_tre/bloom.h"
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

/* Per-TID payload entry (positions + bloom).
 *
 * Phase 6: bloom is borrowed from the build-side TID-bloom hash; the
 * builder does not own it and must not free it.  See
 * src/am/ambuild.c::TidBloomEntry. */
typedef struct PayloadEntry
{
    uint64       tid;           /* packed TID */
    uint32      *positions;     /* palloc'd array of position offsets */
    int          n_positions;
    PgTreBloom  *bloom;         /* borrowed; owned by the build state */
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
                         PgTreBloom *tuple_bloom)
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

        /*
         * Borrow the per-TID bloom; the build-side TID-bloom hash
         * owns the storage and outlives the builder.  When
         * with_payload is true the caller must always supply a
         * bloom -- a NULL here means a build-side bug.
         */
        Assert(tuple_bloom != NULL);
        pe->bloom = tuple_bloom;
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
        XLogRegisterBuffer(0, buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_POSTING_INSERT);
        PageSetLSN(page, recptr);
    }

    UnlockReleaseBuffer(buf);
    return blkno;
}

/*
 * Serialize the payload array into a compact byte stream.
 *
 * Format v6 wire layout for each TID (in sparsemap order):
 *   [n_pos:u16] [positions:u32 * n_pos] [m_code:u8] [k:u8]
 *   [bits: ceil(m/8) bytes]
 * Each entry is MAXALIGN-padded so subsequent u16 reads stay aligned
 * regardless of the variable bit-array size.  Trailing pad bytes are
 * zeroed so reproducing the bytes from a fresh build is
 * deterministic for diff/audit tooling.
 *
 * Returns palloc'd buffer; caller must pfree.
 */
static uint8 *
serialize_payload(PayloadEntry *payload, int payload_count, Size *out_size)
{
    Size estimate = 0;
    uint8 *buf, *ptr;
    int i;

    /*
     * Estimate size: sum MAXALIGN(per-entry size) over all entries.
     * Per entry: 2 (n_pos) + 4*n_pos + 2 (m_code,k) + ceil(m/8) bits.
     */
    for (i = 0; i < payload_count; i++)
    {
        PayloadEntry *pe = &payload[i];
        uint16 m_bits = (pe->bloom != NULL) ? pe->bloom->m_bits
                                            : (uint16) pg_tre_bloom_tuple_bits;
        Size bloom_bytes = (m_bits + 7) / 8;

        estimate += MAXALIGN((Size) 2 + (Size) pe->n_positions * sizeof(uint32)
                             + 2 + bloom_bytes);
    }

    buf = (uint8 *) palloc0(estimate);
    ptr = buf;

    for (i = 0; i < payload_count; i++)
    {
        PayloadEntry *pe = &payload[i];
        uint16 n = (uint16) pe->n_positions;
        uint16 m_bits;
        uint8  m_code;
        uint8  k;
        Size   bloom_bytes;
        Size   raw_size;
        uint8 *entry_start = ptr;

        Assert(pe->bloom != NULL);
        m_bits = pe->bloom->m_bits;
        m_code = pg_tre_bloom_code_for_m(m_bits);
        k      = pe->bloom->k;
        bloom_bytes = (m_bits + 7) / 8;

        /* n_pos */
        memcpy(ptr, &n, sizeof(uint16));
        ptr += sizeof(uint16);

        /* positions (no delta coding for now) */
        if (n > 0 && pe->positions != NULL)
        {
            memcpy(ptr, pe->positions, (Size) n * sizeof(uint32));
            ptr += (Size) n * sizeof(uint32);
        }

        /* [m_code, k] */
        *ptr++ = m_code;
        *ptr++ = k;

        /* bit array */
        memcpy(ptr, pg_tre_bloom_bits(pe->bloom), bloom_bytes);
        ptr += bloom_bytes;

        /* MAXALIGN-pad: subsequent entries' u16 reads stay aligned. */
        raw_size = (Size) (ptr - entry_start);
        ptr = entry_start + MAXALIGN(raw_size);
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
        sparsemap_t *fresh = sm_create(cap);
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
                sparsemap_t *slice_smap = sm_create(cap);
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

    /*
     * Free payload entries (Phase 5).  Note: pe->bloom is borrowed
     * from the build-side TID-bloom hash (see
     * src/am/ambuild.c::TidBloomEntry); the builder must not free
     * it.
     */
    if (b->payload != NULL)
    {
        int i;
        for (i = 0; i < b->payload_count; i++)
        {
            if (b->payload[i].positions != NULL)
                pfree(b->payload[i].positions);
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

sparsemap_t *
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
        sparsemap_t *sm = sm_open_copy(inline_data, inline_bytes, 64);
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
        Size    bytes = hdr->sparsemap_bytes;
        sparsemap_t *sm = sm_open_copy(sm_bytes, bytes, 64);

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
            Size next_bytes = next_hdr->sparsemap_bytes;

            /* Materialize this leaf's sparsemap. */
            sparsemap_t *next_sm = sm_open_copy(next_sm_bytes, next_bytes, 64);
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
            sparsemap_t *merged = sm_union(sm, next_sm);
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
 * Walk a v6 posting-leaf payload to the entry at sparsemap rank
 * `target_rank` (1-indexed: rank == 1 means the first entry).
 * Each v6 entry is
 *     [n_pos:u16] [positions:u32 * n_pos] [m_code:u8] [k:u8]
 *     [bits: ceil(m/8)]
 * MAXALIGN-padded so the subsequent entry's u16 read stays aligned.
 *
 * Returns a pointer to the start of the target entry, or NULL if
 * the payload region was too small.  Sets *out_n_pos / *out_m_bits /
 * *out_k / *out_bits to fields read from the entry header on
 * success.
 */
static const uint8 *
v6_walk_payload_to_rank(const uint8 *payload_base, Size payload_bytes,
                        size_t target_rank,
                        uint16 *out_n_pos, uint16 *out_m_bits,
                        uint8 *out_k, const uint8 **out_bits)
{
    const uint8 *p = payload_base;
    const uint8 *end = payload_base + payload_bytes;
    size_t i;

    if (target_rank == 0)
        return NULL;

    /* Skip (target_rank - 1) entries. */
    for (i = 0; i + 1 < target_rank; i++)
    {
        const uint8 *entry_start = p;
        uint16 n_pos;
        uint8  m_code;
        uint16 m_bits;
        Size   bloom_bytes;
        Size   raw_size;

        if (p + sizeof(uint16) > end)
            return NULL;
        memcpy(&n_pos, p, sizeof(uint16));
        p += sizeof(uint16);
        p += (Size) n_pos * sizeof(uint32);
        if (p + 2 > end)
            return NULL;
        m_code = *p++;
        /* k */ p++;
        m_bits = pg_tre_bloom_m_for_code(m_code);
        bloom_bytes = (m_bits + 7) / 8;
        p += bloom_bytes;
        if (p > end)
            return NULL;

        raw_size = (Size) (p - entry_start);
        p = entry_start + MAXALIGN(raw_size);
        if (p > end)
            return NULL;
    }

    /* Target entry: parse header and surface fields. */
    {
        const uint8 *entry_start = p;
        uint16 n_pos;
        uint8  m_code;
        uint8  k;
        uint16 m_bits;

        if (p + sizeof(uint16) > end)
            return NULL;
        memcpy(&n_pos, p, sizeof(uint16));
        p += sizeof(uint16);
        p += (Size) n_pos * sizeof(uint32);
        if (p + 2 > end)
            return NULL;
        m_code = *p++;
        k      = *p++;
        m_bits = pg_tre_bloom_m_for_code(m_code);
        if (p + (m_bits + 7) / 8 > end)
            return NULL;

        if (out_n_pos)  *out_n_pos  = n_pos;
        if (out_m_bits) *out_m_bits = m_bits;
        if (out_k)      *out_k      = k;
        if (out_bits)   *out_bits   = p;
        return entry_start;
    }
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
 * sized for the largest possible width: max(GUC pg_tre.bloom_tuple_bits,
 * PG_TRE_BLOOM_MAX_M_BITS)) and surfaces the on-disk (m_bits, k)
 * via out_m_bits / out_k.
 */
bool
pg_tre_posting_lookup_tuple_bloom(Relation index,
                                  BlockNumber root,
                                  const uint8 *inline_data,
                                  Size inline_bytes,
                                  uint64 packed_tid,
                                  uint8 *out_bloom,
                                  Size out_bloom_sz,
                                  uint16 *out_m_bits,
                                  uint8 *out_k,
                                  uint32 *out_page_format_version)
{
    sparsemap_t *smap = NULL;
    size_t rank;
    const uint8 *payload_base;

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
        uint32 page_fmt;
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
        sm_bytes = (uint8 *) hdr + MAXALIGN(sizeof(*hdr));
        page_fmt = PageTreGetOpaque(page)->format_version;

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

        payload_base = (const uint8 *) page + hdr->payload_offset;

        if (page_fmt >= 6)
        {
            /* v6: variable-width per-tuple blooms. */
            uint16 n_pos;
            uint16 m_bits;
            uint8  k;
            const uint8 *bits;
            const uint8 *entry;
            Size bloom_bytes;

            entry = v6_walk_payload_to_rank(payload_base,
                                            hdr->payload_bytes,
                                            rank,
                                            &n_pos, &m_bits, &k, &bits);
            if (entry == NULL)
            {
                UnlockReleaseBuffer(buf);
                return false;
            }

            bloom_bytes = (m_bits + 7) / 8;
            if (bloom_bytes > out_bloom_sz)
            {
                /* Caller's buffer is too small for this width. */
                UnlockReleaseBuffer(buf);
                return false;
            }
            memcpy(out_bloom, bits, bloom_bytes);
            if (out_m_bits) *out_m_bits = m_bits;
            if (out_k)      *out_k      = k;
            found = true;
        }
        else
        {
            /* v3..v5: fixed-width bloom; (m_bits, k) come from GUCs. */
            Size bloom_bytes = (pg_tre_bloom_tuple_bits + 7) / 8;
            const uint8 *entry_ptr = payload_base;
            const uint8 *end = payload_base + hdr->payload_bytes;
            size_t i;

            Assert(out_bloom_sz >= bloom_bytes);

            for (i = 0; i + 1 < rank; i++)
            {
                uint16 n_pos;
                if (entry_ptr + sizeof(uint16) > end)
                {
                    UnlockReleaseBuffer(buf);
                    return false;
                }
                memcpy(&n_pos, entry_ptr, sizeof(uint16));
                entry_ptr += sizeof(uint16);
                entry_ptr += (Size) n_pos * sizeof(uint32);
                entry_ptr += bloom_bytes;
                if (entry_ptr > end)
                {
                    UnlockReleaseBuffer(buf);
                    return false;
                }
            }

            {
                uint16 n_pos;
                if (entry_ptr + sizeof(uint16) > end)
                {
                    UnlockReleaseBuffer(buf);
                    return false;
                }
                memcpy(&n_pos, entry_ptr, sizeof(uint16));
                entry_ptr += sizeof(uint16);
                entry_ptr += (Size) n_pos * sizeof(uint32);

                if (entry_ptr + bloom_bytes > end)
                {
                    UnlockReleaseBuffer(buf);
                    return false;
                }
                memcpy(out_bloom, entry_ptr, bloom_bytes);
                if (out_m_bits) *out_m_bits = (uint16) pg_tre_bloom_tuple_bits;
                if (out_k)      *out_k      = 5;
                found = true;
            }
        }

        /*
         * Surface the leaf page's per-page format_version so that the
         * caller's bloom decoder can dispatch on it (see the format-
         * version-aware decode helper in src/util/bloom.c).
         */
        if (out_page_format_version != NULL)
            *out_page_format_version = page_fmt;

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
    sparsemap_t *smap = NULL;
    size_t rank;
    const uint8 *payload_base;
    uint16 n_positions = 0;
    const uint32 *positions_ptr = NULL;

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

        /* Check if this leaf has payload */
        if (hdr->payload_offset == 0)
        {
            UnlockReleaseBuffer(buf);
            return 0;  /* no payload */
        }

        /* Wrap sparsemap */
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

        if (PageTreGetOpaque(page)->format_version >= 6)
        {
            /* v6: variable-width per-tuple blooms; the wire format
             * still puts positions immediately after n_pos so the
             * v6 walker exposes the same starting pointer. */
            uint16 entry_n_pos;
            uint16 m_bits;
            uint8  k;
            const uint8 *bits_unused;
            const uint8 *entry =
                v6_walk_payload_to_rank(payload_base,
                                        hdr->payload_bytes,
                                        rank,
                                        &entry_n_pos, &m_bits, &k,
                                        &bits_unused);

            (void) k;
            (void) bits_unused;
            (void) m_bits;

            if (entry == NULL)
            {
                UnlockReleaseBuffer(buf);
                return 0;
            }

            n_positions = entry_n_pos;
            if (n_positions > 1024)
                n_positions = 1024;
            positions_ptr = (const uint32 *) (entry + sizeof(uint16));
        }
        else
        {
            /* Pre-v6: fixed-width bloom; walk packed entries. */
            Size bloom_bytes = (pg_tre_bloom_tuple_bits + 7) / 8;
            const uint8 *entry_ptr = payload_base;
            size_t entry_idx;

            for (entry_idx = 0; entry_idx + 1 < rank; entry_idx++)
            {
                uint16 entry_n_positions;
                memcpy(&entry_n_positions, entry_ptr, sizeof(uint16));
                entry_ptr += sizeof(uint16);
                entry_ptr += entry_n_positions * sizeof(uint32);
                entry_ptr += bloom_bytes;
            }

            memcpy(&n_positions, entry_ptr, sizeof(uint16));
            entry_ptr += sizeof(uint16);
            if (n_positions > 1024)
                n_positions = 1024;
            positions_ptr = (const uint32 *) entry_ptr;
        }

        /* Copy positions to thread-local buffer */
        memcpy(positions_buf, positions_ptr, n_positions * sizeof(uint32));
        *out_positions = positions_buf;

        UnlockReleaseBuffer(buf);
        return (int) n_positions;
    }
}
