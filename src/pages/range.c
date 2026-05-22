/*
 * src/pages/range.c - BRIN-style range summary tier (tier 1).
 *
 * Phase 5: single-leaf implementation.  Each range entry covers
 * pg_tre_range_size_blocks heap blocks and carries a bloom filter
 * summarizing all trigrams present in that range.
 *
 * The range tree is keyed by range_start_blk and provides fast rejection
 * of entire heap regions during scan.  Phase 8 will extend this to a
 * multi-level tree; Phase 5 keeps it simple with a single leaf page.
 */

#include "postgres.h"

#include <string.h>

#include "access/xlog.h"
#include "access/xloginsert.h"
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
    char *entry_area;
    Size entry_offset;
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
        sparsemap_t *smap;
        uint64 min_tid, max_tid;

        /* Materialize the posting's sparsemap. */
        smap = pg_tre_posting_materialize(index, posting_root, inline_data, inline_bytes);
        if (smap == NULL || sm_cardinality(smap) == 0)
        {
            if (smap != NULL)
                free(smap);
            continue;
        }

        min_tid = sm_minimum(smap);
        max_tid = sm_maximum(smap);

        /* Iterate over this posting's TIDs and union into range blooms. */
        {
            uint64 tid;
            for (tid = min_tid; tid <= max_tid; tid++)
            {
                if (!sm_contains(smap, tid))
                    continue;

                BlockNumber heap_blk = (BlockNumber) (tid >> 16);
                BlockNumber range_start = (heap_blk / range_size) * range_size;
                BlockNumber range_end = range_start + range_size;
                RangeAccum *ra = NULL;

                /* Find or create range accumulator. */
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

                if (tid == max_tid)
                    break;  /* guard against wrap */
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

    /* Allocate a single range leaf page. */
    rangebuf = pg_tre_extend(index, PG_TRE_PAGE_RANGE);
    rangepage = BufferGetPage(rangebuf);
    rangeblk = BufferGetBlockNumber(rangebuf);

    /* Write range entries. */
    entry_area = (char *) PageGetContents(rangepage);
    entry_offset = 0;

    for (i = 0; i < n_ranges; i++)
    {
        PgTreRangeLeafEntry *entry;
        Size bloom_bytes = (meta.bloom_range_m_bits + 7) / 8;
        Size entry_size = sizeof(PgTreRangeLeafEntry) + bloom_bytes;

        if (entry_offset + entry_size > BLCKSZ - MAXALIGN(sizeof(PageTreOpaqueData)))
        {
            /* Out of space: Phase 8 will handle multi-leaf trees. */
            ereport(WARNING,
                    (errmsg("pg_tre: range leaf full after %d entries, truncating", i),
                     errhint("Phase 8 will support multi-leaf range trees.")));
            break;
        }

        entry = (PgTreRangeLeafEntry *) (entry_area + entry_offset);
        entry->range_start_blk = ranges[i].range_start;
        entry->range_end_blk = ranges[i].range_end;
        entry->bloom_bytes = bloom_bytes;

        /* Copy bloom bits inline. */
        memcpy((uint8 *) entry + sizeof(PgTreRangeLeafEntry),
               pg_tre_bloom_bits(ranges[i].bloom),
               bloom_bytes);

        entry_offset += entry_size;
    }

    ((PageHeader) rangepage)->pd_lower = entry_area + entry_offset - (char *) rangepage;

    MarkBufferDirty(rangebuf);

    if (RelationNeedsWAL(index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, rangebuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_RANGE_UPDATE);
        PageSetLSN(rangepage, recptr);
    }

    UnlockReleaseBuffer(rangebuf);

    /* Clean up. */
    for (i = 0; i < n_ranges; i++)
        pfree(ranges[i].bloom);
    pfree(ranges);

    MemoryContextSwitchTo(old_cxt);
    MemoryContextDelete(range_cxt);

    ereport(NOTICE,
            (errmsg("pg_tre: built range tier with %d ranges", n_ranges)));

    return rangeblk;
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
    Buffer rangebuf;
    Page rangepage;
    char *entry_area;
    Size entry_offset;
    int range_size;
    BlockNumber target_range_start;

    pg_tre_meta_read(index, &meta);

    if (meta.root_range == InvalidBlockNumber)
        return false;

    range_size = pg_tre_range_size_blocks;
    target_range_start = (heap_blk / range_size) * range_size;

    rangebuf = pg_tre_read(index, meta.root_range, PG_TRE_PAGE_RANGE,
                           BUFFER_LOCK_SHARE);
    rangepage = BufferGetPage(rangebuf);
    entry_area = (char *) PageGetContents(rangepage);
    entry_offset = 0;

    /* Linear scan for target range (Phase 8 will binary search). */
    while (entry_offset < ((PageHeader) rangepage)->pd_lower -
                          ((char *) rangepage - entry_area))
    {
        PgTreRangeLeafEntry *entry =
            (PgTreRangeLeafEntry *) (entry_area + entry_offset);

        if (entry->range_start_blk == target_range_start)
        {
            /* Found: allocate bloom and copy bits. */
            Size bloom_size = pg_tre_bloom_size_bytes(meta.bloom_range_m_bits);
            PgTreBloom *bloom = (PgTreBloom *) palloc(bloom_size);

            pg_tre_bloom_init(bloom, meta.bloom_range_m_bits, meta.bloom_range_k);
            memcpy(pg_tre_bloom_bits(bloom),
                   (uint8 *) entry + sizeof(PgTreRangeLeafEntry),
                   entry->bloom_bytes);

            *out_bloom = bloom;
            UnlockReleaseBuffer(rangebuf);
            return true;
        }

        entry_offset += sizeof(PgTreRangeLeafEntry) + entry->bloom_bytes;
    }

    UnlockReleaseBuffer(rangebuf);
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
    Buffer rangebuf;
    Page rangepage;
    char *entry_area;
    Size entry_offset;

    pg_tre_meta_read(index, &meta);

    if (meta.root_range == InvalidBlockNumber)
        return;  /* No ranges */

    rangebuf = pg_tre_read(index, meta.root_range, PG_TRE_PAGE_RANGE,
                           BUFFER_LOCK_SHARE);
    rangepage = BufferGetPage(rangebuf);
    entry_area = (char *) PageGetContents(rangepage);
    entry_offset = 0;

    /* Iterate all entries. */
    while (entry_offset < ((PageHeader) rangepage)->pd_lower -
                          ((char *) rangepage - entry_area))
    {
        PgTreRangeLeafEntry *entry =
            (PgTreRangeLeafEntry *) (entry_area + entry_offset);
        PgTreBloom *bloom;
        Size bloom_size;

        /* Reconstruct bloom for callback. */
        bloom_size = pg_tre_bloom_size_bytes(meta.bloom_range_m_bits);
        bloom = (PgTreBloom *) palloc(bloom_size);
        pg_tre_bloom_init(bloom, meta.bloom_range_m_bits, meta.bloom_range_k);
        memcpy(pg_tre_bloom_bits(bloom),
               (uint8 *) entry + sizeof(PgTreRangeLeafEntry),
               entry->bloom_bytes);

        callback(entry->range_start_blk, entry->range_end_blk, bloom, ctx);

        pfree(bloom);
        entry_offset += sizeof(PgTreRangeLeafEntry) + entry->bloom_bytes;
    }

    UnlockReleaseBuffer(rangebuf);
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
