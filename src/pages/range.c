/*
 * src/pages/range.c - BRIN-style range summary tier (tier 1).
 *
 * Phase 8: multi-leaf implementation.  Range pages chain via
 * PgTreRangeHeader.right_link.  meta.root_range points at the FIRST
 * page of the chain.  Each range entry covers pg_tre_range_size_blocks
 * heap blocks and carries a bloom filter summarizing all trigrams
 * present in that range.
 *
 * Format-version dispatch on read:
 *   v5+  range pages carry a PgTreRangeHeader at the start of their
 *        content area; entries follow.  right_link walks the chain.
 *   v<5  range pages have no header; entries start at PageGetContents()
 *        and pd_lower bounds the entry area.  Single-page only (the
 *        bulkload that wrote them truncated past the first page).
 *
 * The range tree is keyed by range_start_blk and provides fast rejection
 * of entire heap regions during scan.  Phase 8 keeps the chain ordered
 * by range_start across pages, so a future binary-search reader can
 * land on the right page in O(log n_pages).
 */

#include "postgres.h"

#include <string.h>

#include "access/xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lockdefs.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "pg_tre/bloom.h"
#include "pg_tre/buffer.h"
#include "pg_tre/meta.h"
#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/posting.h"
#include "pg_tre/range.h"
#include "pg_tre/sparsemap.h"
#include "pg_tre/upper.h"
#include "pg_tre/xlog.h"

/*
 * Range accumulator during bulkload.  Collects trigrams for each range
 * and unions them into a range-level bloom.
 */
typedef struct RangeAccum
{
    BlockNumber range_start;
    BlockNumber range_end;     /* exclusive */
    PgTreBloom *bloom;         /* palloc'd */
} RangeAccum;

/*
 * Comparison function for sorting ranges by range_start.
 */
static int
compare_range_start(const void *a, const void *b)
{
    const RangeAccum *ra = (const RangeAccum *) a;
    const RangeAccum *rb = (const RangeAccum *) b;

    if (ra->range_start < rb->range_start)
        return -1;
    if (ra->range_start > rb->range_start)
        return 1;
    return 0;
}

/*
 * Per-page content capacity: bytes available between the end of
 * PageHeaderData (where PageGetContents() lands) and the start of the
 * special area (where PageTreOpaqueData lives).
 */
static inline Size
range_page_content_capacity(Page page)
{
    return ((PageHeader) page)->pd_special - MAXALIGN(SizeOfPageHeaderData);
}

/*
 * Finalize a range page by stamping its header (v5+) and pd_lower,
 * marking dirty, emitting an FPI if the relation is WAL-logged, and
 * releasing the buffer lock.  next_blk is the right_link target
 * (InvalidBlockNumber for the tail page).
 */
static void
finalize_range_page(Relation index, Buffer rangebuf,
                    Size entry_offset_in_content, int entries_on_page,
                    BlockNumber next_blk)
{
    Page page = BufferGetPage(rangebuf);
    char *content = (char *) PageGetContents(page);
    PgTreRangeHeader *hdr = (PgTreRangeHeader *) content;

    hdr->right_link = next_blk;
    hdr->n_entries  = (uint16) entries_on_page;
    hdr->_pad       = 0;

    ((PageHeader) page)->pd_lower =
        (LocationIndex) (content + entry_offset_in_content - (char *) page);

    MarkBufferDirty(rangebuf);

    if (RelationNeedsWAL(index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, rangebuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_RANGE_UPDATE);
        PageSetLSN(page, recptr);
    }

    UnlockReleaseBuffer(rangebuf);
}

/*
 * Bulk-load the range tree from posting trees.  Scans all trigrams,
 * groups TIDs by heap block range, unions each range's trigrams into
 * a bloom filter, and writes a single range leaf page.
 *
 * Phase 5: single-leaf implementation.  Returns the range leaf's block
 * number, or InvalidBlockNumber if no ranges were built.
 *
 * Called by ambuild after the upper tree is complete.
 */
BlockNumber
pg_tre_range_bulkload(Relation index, UpperTrigramIterator iter, void *iter_ctx)
{
    MemoryContext range_cxt;
    MemoryContext old_cxt;
    RangeAccum *ranges = NULL;
    int n_ranges = 0;
    int ranges_alloced = 0;
    PgTreMetaPageData meta;
    Size bloom_size;
    uint64 trigram_hash;
    BlockNumber posting_root;
    const uint8 *inline_data;
    Size inline_bytes;
    Buffer rangebuf;
    Page rangepage;
    BlockNumber rangeblk;
    BlockNumber root_blk;
    char *content;
    Size entry_offset;
    int entries_on_page;
    int n_pages;
    Size capacity;
    Size bloom_bytes;
    Size entry_size;
    int range_size;
    int i;

    pg_tre_meta_read(index, &meta);
    range_size = pg_tre_range_size_blocks;
    bloom_size = pg_tre_bloom_size_bytes(meta.bloom_range_m_bits);

    range_cxt = AllocSetContextCreate(CurrentMemoryContext,
                                      "pg_tre range bulkload",
                                      ALLOCSET_DEFAULT_SIZES);
    old_cxt = MemoryContextSwitchTo(range_cxt);

    /* Iterate over all trigrams. */
    while (iter(iter_ctx, &trigram_hash, &posting_root, &inline_data, &inline_bytes))
    {
        sm_t *smap;

        /* Materialize the posting's sparsemap. */
        smap = pg_tre_posting_materialize(index, posting_root, inline_data, inline_bytes);
        if (smap == NULL || sm_cardinality(smap) == 0)
        {
            if (smap != NULL)
                free(smap);
            continue;
        }

        /*
         * Iterate the sparse map's set bits with sm_next_member() and
         * union each TID's heap-block range bloom with this trigram.
         *
         * Earlier this code did `for (tid = min_tid; tid <= max_tid; tid++)`
         * with an sm_contains() probe per step.  For frequent trigrams
         * (which span the entire heap), max_tid - min_tid can be tens
         * of millions while the actual TID count is only ~N_rows --
         * making the build O(blocks * trigrams * 65536) instead of
         * O(N_TIDs).  100k-row builds spent the bulk of CPU here.
         *
         * We also dedupe by range_start within a single trigram: TIDs
         * arrive in sorted order, so consecutive TIDs in the same range
         * map to the same RangeAccum and we only need to add the
         * trigram_hash to its bloom once.
         */
        {
            uint64 tid;
            uint64 prev_idx = SM_IDX_MAX;
            BlockNumber last_range_start = InvalidBlockNumber;
            RangeAccum *last_ra = NULL;
            uint64 iter_count = 0;

            while ((tid = sm_next_member(smap, prev_idx)) != SM_IDX_MAX)
            {
                BlockNumber heap_blk;
                BlockNumber range_start;
                BlockNumber range_end;
                RangeAccum *ra;

                prev_idx = tid;

                /*
                 * Allow this build phase to be cancelled.  Without
                 * this, pg_cancel_backend / pg_terminate_backend are
                 * silently ignored for the entire bulkload.
                 */
                if ((iter_count++ & 0xFFFF) == 0)
                    CHECK_FOR_INTERRUPTS();

                heap_blk = (BlockNumber) (tid >> 16);
                range_start = (heap_blk / range_size) * range_size;
                range_end = range_start + range_size;

                if (last_ra != NULL && range_start == last_range_start)
                    continue;

                /* Find or create range accumulator. */
                ra = NULL;
                for (i = 0; i < n_ranges; i++)
                {
                    if (ranges[i].range_start == range_start)
                    {
                        ra = &ranges[i];
                        break;
                    }
                }

                if (ra == NULL)
                {
                    /* Create new range. */
                    if (n_ranges >= ranges_alloced)
                    {
                        int new_cap = (ranges_alloced == 0) ? 64
                                                            : ranges_alloced * 2;
                        ranges = (RangeAccum *)
                            (ranges == NULL
                             ? palloc(new_cap * sizeof(RangeAccum))
                             : repalloc(ranges, new_cap * sizeof(RangeAccum)));
                        ranges_alloced = new_cap;
                    }
                    ra = &ranges[n_ranges++];
                    ra->range_start = range_start;
                    ra->range_end = range_end;
                    ra->bloom = (PgTreBloom *) palloc(bloom_size);
                    pg_tre_bloom_init(ra->bloom, meta.bloom_range_m_bits,
                                      meta.bloom_range_k);
                }

                /* Add this trigram to the range's bloom. */
                pg_tre_bloom_add_trigram(ra->bloom, trigram_hash);

                last_range_start = range_start;
                last_ra = ra;
            }
        }

        free(smap);
    }

    if (n_ranges == 0)
    {
        MemoryContextSwitchTo(old_cxt);
        MemoryContextDelete(range_cxt);
        return InvalidBlockNumber;
    }

    /* Sort ranges by range_start for deterministic layout. */
    qsort(ranges, n_ranges, sizeof(RangeAccum), compare_range_start);

    bloom_bytes = (meta.bloom_range_m_bits + 7) / 8;
    entry_size  = sizeof(PgTreRangeLeafEntry) + bloom_bytes;

    /*
     * Allocate the first range page (head of the chain).  pg_tre_extend
     * stamps it at PG_TRE_FORMAT_VERSION_LATEST, so we always write the
     * v5 PgTreRangeHeader.  Subsequent pages in the chain are allocated
     * lazily as the current page fills.
     */
    rangebuf = pg_tre_extend(index, PG_TRE_PAGE_RANGE);
    rangepage = BufferGetPage(rangebuf);
    rangeblk  = BufferGetBlockNumber(rangebuf);
    root_blk  = rangeblk;
    content   = (char *) PageGetContents(rangepage);
    capacity  = range_page_content_capacity(rangepage);

    /*
     * Initialize the on-page header in place.  finalize_range_page()
     * will stamp the final right_link / n_entries when we hand off.
     */
    {
        PgTreRangeHeader *hdr = (PgTreRangeHeader *) content;
        hdr->right_link = InvalidBlockNumber;
        hdr->n_entries  = 0;
        hdr->_pad       = 0;
    }
    entry_offset    = sizeof(PgTreRangeHeader);
    entries_on_page = 0;
    n_pages         = 1;

    /*
     * Defend against pathological configurations: if a single entry
     * (header + bloom) cannot fit on an empty page, we'd loop forever
     * allocating new pages.  This means bloom_range_m_bits is too
     * large for the current BLCKSZ.
     */
    if (sizeof(PgTreRangeHeader) + entry_size > capacity)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("pg_tre: range bloom too large for page"),
                 errdetail("entry size %zu exceeds page capacity %zu",
                           sizeof(PgTreRangeHeader) + entry_size, capacity)));

    for (i = 0; i < n_ranges; i++)
    {
        PgTreRangeLeafEntry *entry;

        /* Out of space on the current page: chain to a new one. */
        if (entry_offset + entry_size > capacity)
        {
            Buffer  nextbuf  = pg_tre_extend(index, PG_TRE_PAGE_RANGE);
            BlockNumber next_blk = BufferGetBlockNumber(nextbuf);

            /* Finalize the current page with right_link = next. */
            finalize_range_page(index, rangebuf, entry_offset,
                                entries_on_page, next_blk);

            /* Move on to the new tail page. */
            rangebuf = nextbuf;
            rangepage = BufferGetPage(rangebuf);
            content   = (char *) PageGetContents(rangepage);
            capacity  = range_page_content_capacity(rangepage);

            {
                PgTreRangeHeader *hdr = (PgTreRangeHeader *) content;
                hdr->right_link = InvalidBlockNumber;
                hdr->n_entries  = 0;
                hdr->_pad       = 0;
            }
            entry_offset    = sizeof(PgTreRangeHeader);
            entries_on_page = 0;
            n_pages++;
        }

        entry = (PgTreRangeLeafEntry *) (content + entry_offset);
        entry->range_start_blk = ranges[i].range_start;
        entry->range_end_blk   = ranges[i].range_end;
        entry->bloom_bytes     = bloom_bytes;

        /* Copy bloom bits inline. */
        memcpy((uint8 *) entry + sizeof(PgTreRangeLeafEntry),
               pg_tre_bloom_bits(ranges[i].bloom),
               bloom_bytes);

        entry_offset += entry_size;
        entries_on_page++;
    }

    /* Finalize the tail page (no successor). */
    finalize_range_page(index, rangebuf, entry_offset, entries_on_page,
                        InvalidBlockNumber);

    /* Clean up. */
    for (i = 0; i < n_ranges; i++)
        pfree(ranges[i].bloom);
    pfree(ranges);

    MemoryContextSwitchTo(old_cxt);
    MemoryContextDelete(range_cxt);

    ereport(NOTICE,
            (errmsg("pg_tre: built range tier with %d ranges across %d pages",
                    n_ranges, n_pages)));

    return root_blk;
}

/*
 * Look up the range bloom for a given heap block.  Returns true if found,
 * allocates a PgTreBloom in the caller's memory context and returns it via
 * out_bloom.  Caller must pfree the returned bloom.
 *
 * Phase 5 WRITE implementation.
 * Phase 5 READ will use this for tier-1 filtering.
 */
bool
pg_tre_range_lookup(Relation index, BlockNumber heap_blk, PgTreBloom **out_bloom)
{
    PgTreMetaPageData meta;
    int range_size;
    BlockNumber target_range_start;
    BlockNumber blk;

    pg_tre_meta_read(index, &meta);

    if (meta.root_range == InvalidBlockNumber)
        return false;

    range_size = pg_tre_range_size_blocks;
    target_range_start = (heap_blk / range_size) * range_size;

    /*
     * Walk the right-link chain starting at meta.root_range.  Within
     * each page we linear-scan; ranges are stored in ascending
     * range_start order across the entire chain so we can stop as soon
     * as we pass the target.
     */
    blk = meta.root_range;
    while (blk != InvalidBlockNumber)
    {
        Buffer rangebuf;
        Page rangepage;
        char *content;
        Size entries_offset;
        Size entries_end;
        BlockNumber next_blk = InvalidBlockNumber;
        uint32 page_format;

        rangebuf = pg_tre_read(index, blk, PG_TRE_PAGE_RANGE,
                               BUFFER_LOCK_SHARE);
        rangepage = BufferGetPage(rangebuf);
        content   = (char *) PageGetContents(rangepage);
        page_format = PageTreGetOpaque(rangepage)->format_version;

        if (page_format >= 5)
        {
            const PgTreRangeHeader *hdr = (const PgTreRangeHeader *) content;
            next_blk       = hdr->right_link;
            entries_offset = sizeof(PgTreRangeHeader);
        }
        else
        {
            /* v3 / v4: no header, single page. */
            entries_offset = 0;
        }

        entries_end = (Size) (((PageHeader) rangepage)->pd_lower) -
                      MAXALIGN(SizeOfPageHeaderData);

        while (entries_offset < entries_end)
        {
            const PgTreRangeLeafEntry *entry =
                (const PgTreRangeLeafEntry *) (content + entries_offset);

            if (entry->range_start_blk == target_range_start)
            {
                Size bloom_size = pg_tre_bloom_size_bytes(meta.bloom_range_m_bits);
                PgTreBloom *bloom;

                /* Copy bloom bits while we still hold the page lock. */
                bloom = (PgTreBloom *) palloc(bloom_size);
                pg_tre_bloom_init(bloom, meta.bloom_range_m_bits,
                                  meta.bloom_range_k);
                memcpy(pg_tre_bloom_bits(bloom),
                       (const uint8 *) entry + sizeof(PgTreRangeLeafEntry),
                       entry->bloom_bytes);

                UnlockReleaseBuffer(rangebuf);
                *out_bloom = bloom;
                return true;
            }

            /*
             * Ranges are sorted ascending across the chain.  Once we
             * pass the target we can stop the whole scan.
             */
            if (entry->range_start_blk > target_range_start)
            {
                UnlockReleaseBuffer(rangebuf);
                return false;
            }

            entries_offset += sizeof(PgTreRangeLeafEntry) + entry->bloom_bytes;
        }

        UnlockReleaseBuffer(rangebuf);
        blk = next_blk;
    }

    return false;
}

/*
 * Iterate all range entries.  Calls the callback for each range in
 * ascending block order.
 *
 * Phase 5 WRITE implementation for debug introspection.
 * Phase 5 READ will use this for tier-1 block-mask construction.
 */
void
pg_tre_range_scan(Relation index, PgTreRangeScanCallback callback, void *ctx)
{
    PgTreMetaPageData meta;
    BlockNumber blk;

    pg_tre_meta_read(index, &meta);

    if (meta.root_range == InvalidBlockNumber)
        return;  /* No ranges */

    /* Walk the right-link chain. */
    blk = meta.root_range;
    while (blk != InvalidBlockNumber)
    {
        Buffer rangebuf;
        Page rangepage;
        char *content;
        Size entries_offset;
        Size entries_end;
        BlockNumber next_blk = InvalidBlockNumber;
        uint32 page_format;

        rangebuf = pg_tre_read(index, blk, PG_TRE_PAGE_RANGE,
                               BUFFER_LOCK_SHARE);
        rangepage = BufferGetPage(rangebuf);
        content   = (char *) PageGetContents(rangepage);
        page_format = PageTreGetOpaque(rangepage)->format_version;

        if (page_format >= 5)
        {
            const PgTreRangeHeader *hdr = (const PgTreRangeHeader *) content;
            next_blk       = hdr->right_link;
            entries_offset = sizeof(PgTreRangeHeader);
        }
        else
        {
            /* v3 / v4: no header, single page (root only). */
            entries_offset = 0;
        }

        entries_end = (Size) (((PageHeader) rangepage)->pd_lower) -
                      MAXALIGN(SizeOfPageHeaderData);

        while (entries_offset < entries_end)
        {
            const PgTreRangeLeafEntry *entry =
                (const PgTreRangeLeafEntry *) (content + entries_offset);
            BlockNumber range_start = entry->range_start_blk;
            BlockNumber range_end   = entry->range_end_blk;
            Size bloom_size = pg_tre_bloom_size_bytes(meta.bloom_range_m_bits);
            PgTreBloom *bloom;

            /* Reconstruct bloom for callback while we hold the lock. */
            bloom = (PgTreBloom *) palloc(bloom_size);
            pg_tre_bloom_init(bloom, meta.bloom_range_m_bits,
                              meta.bloom_range_k);
            memcpy(pg_tre_bloom_bits(bloom),
                   (const uint8 *) entry + sizeof(PgTreRangeLeafEntry),
                   entry->bloom_bytes);

            entries_offset += sizeof(PgTreRangeLeafEntry) + entry->bloom_bytes;

            callback(range_start, range_end, bloom, ctx);
            pfree(bloom);
        }

        UnlockReleaseBuffer(rangebuf);
        blk = next_blk;
    }
}

/*
 * Rebuild the range tree from scratch.  Used by pending-list merge
 * (Phase 4) after the upper tree is rebuilt.
 *
 * Phase 5: stub.  Phase 8 will optimize to delta updates.
 */
void
pg_tre_range_rebuild(Relation index)
{
    /* Phase 4 merge doesn't call this yet.  Phase 5 READ will complete. */
    (void) index;
}
