/*
 * src/pages/coalesced.c - coalesced posting page writer/reader (format
 * v8, posting-page coalescing).
 *
 * A coalesced page packs the postings (serialized sparsemap + optional
 * payload) of multiple trigrams onto one 8 KB page.  Each trigram's
 * postings occupy a slot; the slot index is recorded in the upper-tree
 * leaf entry (inline_bytes = PG_TRE_COALESCED_FLAG | slot_idx).  This
 * collapses the long tail of medium-cardinality trigrams that today
 * each waste a mostly-empty dedicated posting leaf.
 *
 * See doc/specs/posting-page-coalescing.md for the on-disk layout and
 * the additive (v7 -> v8, no REINDEX) format strategy.
 *
 * WAL: each page is written once, fully, and logged as a full-page
 * image under XLOG_PTRE_POSTING_INSERT (the same op code and generic
 * pg_tre_redo_fpi path the dedicated posting leaves use).  We follow
 * the validated run-catalog writer discipline: REGBUF_FORCE_IMAGE
 * (NEVER REGBUF_WILL_INIT -- it implies NO_IMAGE and PANICs the generic
 * FPI redo), all page edits inside one critical section, PageSetLSN
 * before END_CRIT_SECTION.  pg_tre_extend physically extends the
 * relation, so the block exists at replay.
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
#include "pg_tre/coalesced.h"
#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/xlog.h"

/* Max slots per page given the budget and a minimum per-slot blob. */
#define COALESCED_MAX_SLOTS \
    (pg_tre_coalesced_budget() / sizeof(PgTreCoalescedSlot))

struct PgTreCoalescedWriter
{
    Relation    index;

    /* Page currently being filled (InvalidBuffer when none open). */
    Buffer      buf;
    Page        page;
    PgTreCoalescedHeader *hdr;
    PgTreCoalescedSlot   *slots;     /* indirection table base on page */
};

/*
 * Largest single slot (table entry + MAXALIGN'd sparsemap + MAXALIGN'd
 * payload) that can ever fit on an empty coalesced page.  Used only to
 * reject a blob too large for any page.
 */
static inline Size
slot_footprint(Size sm_len, Size payload_len)
{
    Size n = sizeof(PgTreCoalescedSlot)
           + MAXALIGN(sm_len);
    if (payload_len > 0)
        n += MAXALIGN(payload_len);
    return n;
}

PgTreCoalescedWriter *
pg_tre_coalesced_writer_begin(Relation index)
{
    PgTreCoalescedWriter *w = (PgTreCoalescedWriter *) palloc0(sizeof(*w));
    w->index = index;
    w->buf = InvalidBuffer;
    return w;
}

/*
 * Initialize a freshly-extended coalesced page for filling.
 *
 * Two-ended layout (heap-page style): the indirection table grows UP
 * from just past the header, and blobs grow DOWN from the end of the
 * usable content area (just before the opaque trailer).  The free hole
 * lies between them.  free_offset tracks the low edge of the blob
 * region (== the lowest blob start written so far).  Growing the table
 * and the blobs from opposite ends makes a table/blob collision
 * impossible: a new table entry only ever extends pd_lower upward, and
 * a new blob only ever extends the blob region downward; the per-add
 * guard rolls to a fresh page before the two regions would meet.
 *
 * (The pre-2.0.2 writer grew both from the low end, placing slot 0's
 * blob exactly where slot 1's table entry would later be written --
 * the table entry then clobbered the head of slot 0's blob.  That bug
 * was latent while pages rarely held >1 slot; it surfaced once the
 * coalesce band widened to pack 2-3 slots per page.)
 */
static void
open_page(PgTreCoalescedWriter *w)
{
    w->buf = pg_tre_extend(w->index, PG_TRE_PAGE_POSTING_COALESCED);
    w->page = BufferGetPage(w->buf);
    w->hdr = (PgTreCoalescedHeader *) PageGetContents(w->page);
    w->hdr->n_slots = 0;
    /* free_offset = low edge of the (initially empty) blob region: the
     * top of the usable content area, i.e. just below the opaque
     * trailer.  Blobs are placed by subtracting from this. */
    w->hdr->free_offset = (uint16) (BLCKSZ - MAXALIGN(sizeof(PageTreOpaqueData)));
    w->hdr->_pad0 = 0;
    w->slots = (PgTreCoalescedSlot *)
        (((char *) PageGetContents(w->page)) + MAXALIGN(sizeof(PgTreCoalescedHeader)));
}

/*
 * Finalize the open page: set pd_lower past the indirection table and
 * pd_upper to the low edge of the blob region (free_offset).  The hole
 * between pd_lower and pd_upper is the only stripped region under
 * REGBUF_STANDARD; the table (below pd_lower) and the blobs (at/above
 * pd_upper) both survive FPI hole-stripping.  WAL-log and release.
 */
static void
flush_page(PgTreCoalescedWriter *w)
{
    if (w->buf == InvalidBuffer)
        return;

    /* Edits already made under no lock conflict (we hold the buffer
     * exclusive from pg_tre_extend); do the dirty + WAL atomically. */
    START_CRIT_SECTION();

    ((PageHeader) w->page)->pd_lower =
        (uint16) (MAXALIGN(SizeOfPageHeaderData)
                  + MAXALIGN(sizeof(PgTreCoalescedHeader))
                  + (Size) w->hdr->n_slots * sizeof(PgTreCoalescedSlot));
    ((PageHeader) w->page)->pd_upper = w->hdr->free_offset;

    MarkBufferDirty(w->buf);

    if (RelationNeedsWAL(w->index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, w->buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_POSTING_INSERT);
        PageSetLSN(w->page, recptr);
    }

    END_CRIT_SECTION();

    UnlockReleaseBuffer(w->buf);
    w->buf = InvalidBuffer;
    w->page = NULL;
    w->hdr = NULL;
    w->slots = NULL;
}

void
pg_tre_coalesced_add(PgTreCoalescedWriter *w,
                     uint64 trigram_hash,
                     const uint8 *sm_data, Size sm_len,
                     const uint8 *payload_data, Size payload_len,
                     BlockNumber *out_blk, uint16 *out_slot)
{
    Size        need = slot_footprint(sm_len, payload_len);
    Size        budget = pg_tre_coalesced_budget();
    PgTreCoalescedSlot *slot;
    Size        sm_at;
    Size        payload_at = 0;

    if (sm_len == 0 || sm_len > PG_UINT16_MAX || payload_len > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("pg_tre: coalesced slot blob too large "
                        "(sparsemap %zu, payload %zu)", sm_len, payload_len)));
    if (need > budget)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("pg_tre: coalesced posting for trigram %016llx "
                        "(%zu bytes) exceeds page budget %zu",
                        (unsigned long long) trigram_hash, need, budget)));

    /*
     * Open a page if none is current.
     */
    if (w->buf == InvalidBuffer)
        open_page(w);

    /*
     * Two-ended placement.  The blob region grows DOWN from free_offset
     * (the current low edge of the blobs, initialized to the top of the
     * usable content area).  The indirection table grows UP; after this
     * append it ends at table_end.  Roll to a fresh page if the new
     * blob's bottom would drop below the table's (post-append) top, or
     * if the slot-count cap is hit.
     */
    {
        Size content_start = MAXALIGN(SizeOfPageHeaderData)
                           + MAXALIGN(sizeof(PgTreCoalescedHeader));
        Size align = (Size) (MAXIMUM_ALIGNOF - 1);
        Size table_end = content_start
                       + (Size) (w->hdr->n_slots + 1) * sizeof(PgTreCoalescedSlot);
        Size top = (Size) w->hdr->free_offset;
        Size sm_lo;
        Size blob_lo;

        /* sparsemap occupies [sm_lo, sm_lo + sm_len), MAXALIGN'd down. */
        sm_lo = (top - sm_len) & ~align;
        blob_lo = sm_lo;
        if (payload_len > 0)
            blob_lo = (sm_lo - payload_len) & ~align;

        /* Underflow guard: if sm_len/payload_len exceed top the
         * subtraction wraps; detect via blob_lo < content_start or a
         * wrap that puts blob_lo above top. */
        if (blob_lo < table_end
            || blob_lo > top
            || w->hdr->n_slots >= COALESCED_MAX_SLOTS)
        {
            flush_page(w);
            open_page(w);

            /* Recompute on the fresh (empty) page. */
            table_end = content_start + (Size) 1 * sizeof(PgTreCoalescedSlot);
            top = (Size) w->hdr->free_offset;
            sm_lo = (top - sm_len) & ~align;
            blob_lo = sm_lo;
            if (payload_len > 0)
                blob_lo = (sm_lo - payload_len) & ~align;
        }

        /* Write the blobs at the carved positions. */
        sm_at = sm_lo;
        memcpy((char *) w->page + sm_at, sm_data, sm_len);
        if (payload_len > 0 && payload_data != NULL)
        {
            payload_at = blob_lo;
            memcpy((char *) w->page + payload_at, payload_data, payload_len);
        }
        w->hdr->free_offset = (uint16) blob_lo;
    }

    slot = &w->slots[w->hdr->n_slots];

    slot->sm_offset      = (uint16) sm_at;
    slot->sm_length      = (uint16) sm_len;
    slot->payload_offset = (uint16) payload_at;     /* 0 if no payload */
    slot->payload_length = (uint16) payload_len;
    slot->trigram_hash   = trigram_hash;

    *out_blk  = BufferGetBlockNumber(w->buf);
    *out_slot = w->hdr->n_slots;

    w->hdr->n_slots++;
}

void
pg_tre_coalesced_writer_finish(PgTreCoalescedWriter *w)
{
    flush_page(w);
    pfree(w);
}

/*
 * Validate that a slot's regions stay within the page.  Defends against
 * a torn/corrupt page driving memcpy past the buffer.
 */
static bool
slot_within_page(const PgTreCoalescedSlot *slot)
{
    Size usable_end = BLCKSZ - MAXALIGN(sizeof(PageTreOpaqueData));
    Size content_start = MAXALIGN(SizeOfPageHeaderData)
                       + MAXALIGN(sizeof(PgTreCoalescedHeader));

    if (slot->sm_offset < content_start || slot->sm_offset > usable_end)
        return false;
    if (slot->sm_length > usable_end - slot->sm_offset)
        return false;
    if (slot->payload_offset != 0)
    {
        if (slot->payload_offset < content_start
            || slot->payload_offset > usable_end)
            return false;
        if (slot->payload_length > usable_end - slot->payload_offset)
            return false;
    }
    return true;
}

uint8 *
pg_tre_coalesced_resolve_slot(Relation index, BlockNumber blk,
                              uint16 slot_idx, uint64 trigram_hash,
                              Size *out_len)
{
    Buffer      buf;
    Page        page;
    PgTreCoalescedHeader *hdr;
    PgTreCoalescedSlot   *slots;
    PgTreCoalescedSlot    slot;
    uint8      *copy;

    buf = pg_tre_read(index, blk, PG_TRE_PAGE_POSTING_COALESCED,
                      BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);
    hdr = (PgTreCoalescedHeader *) PageGetContents(page);
    slots = (PgTreCoalescedSlot *)
        (((char *) PageGetContents(page)) + MAXALIGN(sizeof(PgTreCoalescedHeader)));

    if (slot_idx >= hdr->n_slots)
    {
        UnlockReleaseBuffer(buf);
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("pg_tre: coalesced slot %u out of range "
                        "(page %u has %u slots)",
                        slot_idx, blk, hdr->n_slots)));
    }

    slot = slots[slot_idx];

    /* INVALID (tombstoned) slot: no live posting. */
    if (slot.sm_offset == PG_TRE_COALESCED_SM_INVALID)
    {
        UnlockReleaseBuffer(buf);
        *out_len = 0;
        return NULL;
    }

    if (slot.trigram_hash != trigram_hash || !slot_within_page(&slot))
    {
        /*
         * When the caller passes PG_TRE_COALESCED_HASH_ANY it is
         * resolving a marker that came from the index's own upper
         * tree (build/merge/range paths) and does not have the hash
         * handy; the slot's own stored hash is authoritative.  Only
         * the within-page bounds check applies in that case.
         */
        bool hash_ok = (trigram_hash == PG_TRE_COALESCED_HASH_ANY)
                       || (slot.trigram_hash == trigram_hash);
        if (!hash_ok || !slot_within_page(&slot))
        {
            UnlockReleaseBuffer(buf);
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("pg_tre: corrupt coalesced slot %u at block %u "
                            "(slot hash %016llx, expected %016llx)",
                            slot_idx, blk,
                            (unsigned long long) slot.trigram_hash,
                            (unsigned long long) trigram_hash)));
        }
    }

    /* Copy the sparsemap blob out so it survives buffer release. */
    copy = (uint8 *) palloc(slot.sm_length + 8);
    memcpy(copy, (char *) page + slot.sm_offset, slot.sm_length);
    memset(copy + slot.sm_length, 0, 8);

    UnlockReleaseBuffer(buf);
    *out_len = slot.sm_length;
    return copy;
}
