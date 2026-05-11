/*
 * src/am/ambuild.c - index build phase.
 *
 * Phase 2: in-memory sort-based bulk build.
 *
 * Algorithm:
 *   1. Initialize empty meta page
 *   2. Scan heap, extract byte trigrams from indexed text column
 *   3. Hash each trigram to uint64, accumulate (hash, TID) in memory
 *   4. Sort by (hash, TID) using qsort
 *   5. For each run of same hash:
 *      - Accumulate TIDs into a PgTrePostingBuilder
 *      - Call finish() to get inline blob or posting root
 *      - Record (hash, root|inline) in memory
 *   6. Bulk-load upper tree from sorted (hash, root) list
 *   7. Update meta page with root_upper and stats
 *
 * For Phase 2, we skip:
 *   - Positions and tuple blooms (Phase 5)
 *   - Range summary tree (Phase 5)
 *   - Parallel build (Phase 2 optional; Phase 8 refinement)
 *   - Disk-based external sort (Phase 8 for large builds)
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "pg_tre/amapi.h"
#include "pg_tre/bloom.h"
#include "pg_tre/hash.h"
#include "pg_tre/meta.h"
#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/posting.h"
#include "pg_tre/range.h"
#include "pg_tre/upper.h"

/* In-memory sort entry: (trigram_hash, tid, position). */
typedef struct TrigramTidEntry
{
    uint64      trigram_hash;
    ItemPointerData tid;
    uint32      position;       /* byte offset in original text */
} TrigramTidEntry;

/* Per-TID bloom tracking during build. */
typedef struct TidBloomEntry
{
    uint64      packed_tid;
    PgTreBloom *bloom;          /* palloc'd bloom filter */
} TidBloomEntry;

/* Callback context for heap scan. */
typedef struct BuildState
{
    Relation    heap;
    Relation    index;
    IndexInfo  *indexInfo;

    /* In-memory accumulator. */
    TrigramTidEntry *entries;
    int         n_entries;
    int         entries_alloced;

    /* Per-TID bloom tracking (Phase 5). */
    TidBloomEntry *tid_blooms;
    int         n_tid_blooms;
    int         tid_blooms_alloced;

    double      heap_tuples;
    MemoryContext tmpctx;
} BuildState;

/* Accumulator for posting entries during sort readout. */
typedef struct PostingAccum
{
    uint64      trigram_hash;
    BlockNumber root;
    const uint8 *inline_data;
    Size        inline_bytes;
} PostingAccum;

/* State for upper-tree bulkload iterator. */
typedef struct UpperIterState
{
    PostingAccum *accums;
    int         n_accums;
    int         current;
} UpperIterState;

/*
 * qsort comparison function: order by (trigram_hash, tid, position).
 */
static int
compare_trigram_tid(const void *a, const void *b)
{
    const TrigramTidEntry *ea = (const TrigramTidEntry *) a;
    const TrigramTidEntry *eb = (const TrigramTidEntry *) b;

    if (ea->trigram_hash < eb->trigram_hash)
        return -1;
    if (ea->trigram_hash > eb->trigram_hash)
        return 1;

    /* Same hash: order by TID, then position. */
    {
        ItemPointerData tid_a = ea->tid;
        ItemPointerData tid_b = eb->tid;
        int cmp = ItemPointerCompare(&tid_a, &tid_b);
        if (cmp != 0)
            return cmp;

        /* Same TID: order by position. */
        if (ea->position < eb->position)
            return -1;
        if (ea->position > eb->position)
            return 1;
        return 0;
    }
}

/*
 * Find or create a bloom filter for a given TID (Phase 5).
 * Linear search for now; Phase 8 can optimize with a hash table.
 */
static PgTreBloom *
find_or_create_tid_bloom(BuildState *bstate, ItemPointer tid)
{
    uint64 packed = pg_tre_pack_tid(tid);
    int i;

    if (!pg_tre_tuple_bloom_enable)
        return NULL;

    /* Linear search for existing entry. */
    for (i = 0; i < bstate->n_tid_blooms; i++)
    {
        if (bstate->tid_blooms[i].packed_tid == packed)
            return bstate->tid_blooms[i].bloom;
    }

    /* Not found: create new entry. */
    if (bstate->n_tid_blooms >= bstate->tid_blooms_alloced)
    {
        bstate->tid_blooms_alloced *= 2;
        bstate->tid_blooms = (TidBloomEntry *)
            repalloc(bstate->tid_blooms,
                     bstate->tid_blooms_alloced * sizeof(TidBloomEntry));
    }

    {
        TidBloomEntry *tbe = &bstate->tid_blooms[bstate->n_tid_blooms++];
        Size bloom_size = pg_tre_bloom_size_bytes(pg_tre_bloom_tuple_bits);
        PgTreBloom *bloom;

        tbe->packed_tid = packed;
        bloom = (PgTreBloom *) palloc(bloom_size);
        pg_tre_bloom_init(bloom, pg_tre_bloom_tuple_bits, 5 /* k */);
        tbe->bloom = bloom;
        return bloom;
    }
}

/*
 * Extract trigrams from a text datum and insert them into the build state.
 * Phase 5: also populate per-tuple bloom filter and record positions.
 */
static void
extract_trigrams(BuildState *bstate, Datum value, bool isnull, ItemPointer tid)
{
    text       *txt;
    char       *str;
    int         len;
    int         i;
    PgTreBloom *bloom = NULL;

    if (isnull)
        return;

    /* Detoast if needed. */
    txt = DatumGetTextPP(value);
    str = VARDATA_ANY(txt);
    len = VARSIZE_ANY_EXHDR(txt);

    /* Phase 5: get or create the per-TID bloom filter. */
    if (pg_tre_tuple_bloom_enable)
        bloom = find_or_create_tid_bloom(bstate, tid);

    /* Extract byte trigrams: every 3 consecutive bytes. */
    for (i = 0; i <= len - 3; i++)
    {
        uint8   trigram[3];
        uint64  trigram_hash;
        TrigramTidEntry *entry;

        trigram[0] = (uint8) str[i];
        trigram[1] = (uint8) str[i + 1];
        trigram[2] = (uint8) str[i + 2];
        trigram_hash = pg_tre_hash_trigram(trigram);

        /* Grow array if needed. */
        if (bstate->n_entries >= bstate->entries_alloced)
        {
            bstate->entries_alloced *= 2;
            bstate->entries = (TrigramTidEntry *)
                repalloc(bstate->entries,
                         bstate->entries_alloced * sizeof(TrigramTidEntry));
        }

        entry = &bstate->entries[bstate->n_entries++];
        entry->trigram_hash = trigram_hash;
        entry->tid = *tid;
        entry->position = (uint32) i;  /* byte offset */

        /* Phase 5: add trigram to the per-tuple bloom. */
        if (bloom != NULL)
            pg_tre_bloom_add_trigram(bloom, trigram_hash);
    }
}

/*
 * Heap scan callback: extract trigrams from each tuple.
 */
static void
build_callback(Relation index, ItemPointer tid, Datum *values, bool *isnull,
               bool tupleIsAlive, void *state)
{
    BuildState *bstate = (BuildState *) state;

    /*
     * Phase 2: only index the first column (assume it's text).
     * Later phases handle multi-column and operator classes properly.
     */
    if (bstate->indexInfo->ii_NumIndexAttrs >= 1)
    {
        extract_trigrams(bstate, values[0], isnull[0], tid);
    }

    bstate->heap_tuples += 1.0;
}

/*
 * Iterator for upper-tree bulkload.
 */
static bool
upper_iter(void *ctx, uint64 *hash, BlockNumber *root,
           const uint8 **inline_data, Size *inline_bytes)
{
    UpperIterState *state = (UpperIterState *) ctx;

    if (state->current >= state->n_accums)
        return false;

    *hash = state->accums[state->current].trigram_hash;
    *root = state->accums[state->current].root;
    *inline_data = state->accums[state->current].inline_data;
    *inline_bytes = state->accums[state->current].inline_bytes;

    state->current++;
    return true;
}

IndexBuildResult *
pg_tre_ambuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
    IndexBuildResult *result;
    BuildState  bstate;
    MemoryContext oldcxt;
    PostingAccum *accums;
    int         n_accums;
    int         accums_alloced;
    uint64      current_hash;
    PgTrePostingBuilder *current_builder;
    BlockNumber root_upper;
    UpperIterState iter_state;
    int         i;

    /* Phase 2 real build starts here. */

    /* Step 1: initialize empty meta page. */
    pg_tre_build_empty(index);

    /* Step 2: set up in-memory accumulator. */
    bstate.heap = heap;
    bstate.index = index;
    bstate.indexInfo = indexInfo;
    bstate.entries_alloced = 1024 * 1024;  /* start with 1M entries */
    bstate.entries = (TrigramTidEntry *)
        palloc(bstate.entries_alloced * sizeof(TrigramTidEntry));
    bstate.n_entries = 0;

    /* Phase 5: initialize per-TID bloom tracking. */
    bstate.tid_blooms_alloced = 4096;
    bstate.tid_blooms = (TidBloomEntry *)
        palloc(bstate.tid_blooms_alloced * sizeof(TidBloomEntry));
    bstate.n_tid_blooms = 0;

    bstate.heap_tuples = 0.0;
    bstate.tmpctx = AllocSetContextCreate(CurrentMemoryContext,
                                          "pg_tre build temp context",
                                          ALLOCSET_DEFAULT_SIZES);

    /* Step 3: scan heap and collect trigrams. */
    table_index_build_scan(heap, index, indexInfo, true, true,
                           build_callback, &bstate, NULL);

    ereport(NOTICE,
            (errmsg("pg_tre: collected %d trigram entries from %.0f heap tuples",
                    bstate.n_entries, bstate.heap_tuples)));

    /* Step 4: sort by (hash, tid). */
    if (bstate.n_entries > 0)
        qsort(bstate.entries, bstate.n_entries, sizeof(TrigramTidEntry),
              compare_trigram_tid);

    /* Step 5: process sorted entries and build posting trees. */
    oldcxt = MemoryContextSwitchTo(bstate.tmpctx);

    accums_alloced = 1024;
    accums = (PostingAccum *) palloc(accums_alloced * sizeof(PostingAccum));
    n_accums = 0;

    current_hash = 0;
    current_builder = NULL;

    /* Phase 5: accumulate positions per (trigram, TID) pair. */
    {
        uint32 *positions_buf = NULL;
        int positions_alloced = 0;
        int n_positions = 0;
        uint64 current_tid_packed = 0;

        for (i = 0; i < bstate.n_entries; i++)
        {
            TrigramTidEntry *entry = &bstate.entries[i];
            uint64 tid_packed = pg_tre_pack_tid(&entry->tid);
            bool new_trigram = (current_builder == NULL ||
                               entry->trigram_hash != current_hash);
            bool new_tid = (new_trigram || tid_packed != current_tid_packed);

            if (new_trigram)
            {
                /* Finish previous trigram's posting tree. */
                if (current_builder != NULL)
                {
                    /* Add the last accumulated TID. */
                    if (n_positions > 0)
                    {
                        ItemPointerData tid;
                        PgTreBloom *bloom = NULL;
                        uint8 *bloom_bits = NULL;

                        pg_tre_unpack_tid(current_tid_packed, &tid);

                        /* Look up the TID's bloom. */
                        if (pg_tre_tuple_bloom_enable)
                        {
                            int j;
                            for (j = 0; j < bstate.n_tid_blooms; j++)
                            {
                                if (bstate.tid_blooms[j].packed_tid == current_tid_packed)
                                {
                                    bloom = bstate.tid_blooms[j].bloom;
                                    break;
                                }
                            }
                            if (bloom != NULL)
                                bloom_bits = pg_tre_bloom_bits(bloom);
                        }

                        pg_tre_posting_build_add(current_builder, &tid,
                                                positions_buf, n_positions,
                                                bloom_bits);
                    }

                    /* Finish the posting tree. */
                    {
                        BlockNumber root;
                        const uint8 *inline_data;
                        Size        inline_bytes;

                        root = pg_tre_posting_build_finish(current_builder,
                                                           &inline_data,
                                                           &inline_bytes);
                        pg_tre_posting_build_free(current_builder);

                        /* Record the completed posting. */
                        if (n_accums >= accums_alloced)
                        {
                            accums_alloced *= 2;
                            accums = (PostingAccum *)
                                repalloc(accums, accums_alloced * sizeof(PostingAccum));
                        }
                        accums[n_accums].trigram_hash = current_hash;
                        accums[n_accums].root = root;
                        accums[n_accums].inline_data = inline_data;
                        accums[n_accums].inline_bytes = inline_bytes;
                        n_accums++;
                    }
                }

                /* Start a new posting tree. */
                current_hash = entry->trigram_hash;
                current_builder = pg_tre_posting_build_begin(
                                      index, current_hash,
                                      pg_tre_tuple_bloom_enable /* with_payload */);
                n_positions = 0;
            }

            if (new_tid && !new_trigram)
            {
                /* Flush accumulated positions for the previous TID. */
                if (n_positions > 0)
                {
                    ItemPointerData tid;
                    PgTreBloom *bloom = NULL;
                    uint8 *bloom_bits = NULL;

                    pg_tre_unpack_tid(current_tid_packed, &tid);

                    /* Look up the TID's bloom. */
                    if (pg_tre_tuple_bloom_enable)
                    {
                        int j;
                        for (j = 0; j < bstate.n_tid_blooms; j++)
                        {
                            if (bstate.tid_blooms[j].packed_tid == current_tid_packed)
                            {
                                bloom = bstate.tid_blooms[j].bloom;
                                break;
                            }
                        }
                        if (bloom != NULL)
                            bloom_bits = pg_tre_bloom_bits(bloom);
                    }

                    pg_tre_posting_build_add(current_builder, &tid,
                                            positions_buf, n_positions,
                                            bloom_bits);
                    n_positions = 0;
                }
            }

            /* Accumulate this position. */
            current_tid_packed = tid_packed;
            if (pg_tre_tuple_bloom_enable)
            {
                if (n_positions >= positions_alloced)
                {
                    positions_alloced = (positions_alloced == 0) ? 16 :
                                        positions_alloced * 2;
                    positions_buf = (uint32 *) repalloc(positions_buf,
                                      positions_alloced * sizeof(uint32));
                }
                positions_buf[n_positions++] = entry->position;
            }
        }

        /* Finish the last posting tree. */
        if (current_builder != NULL)
        {
            /* Add the last accumulated TID. */
            if (n_positions > 0)
            {
                ItemPointerData tid;
                PgTreBloom *bloom = NULL;
                uint8 *bloom_bits = NULL;

                pg_tre_unpack_tid(current_tid_packed, &tid);

                /* Look up the TID's bloom. */
                if (pg_tre_tuple_bloom_enable)
                {
                    int j;
                    for (j = 0; j < bstate.n_tid_blooms; j++)
                    {
                        if (bstate.tid_blooms[j].packed_tid == current_tid_packed)
                        {
                            bloom = bstate.tid_blooms[j].bloom;
                            break;
                        }
                    }
                    if (bloom != NULL)
                        bloom_bits = pg_tre_bloom_bits(bloom);
                }

                pg_tre_posting_build_add(current_builder, &tid,
                                        positions_buf, n_positions,
                                        bloom_bits);
            }

            /* Finish the posting tree. */
            {
                BlockNumber root;
                const uint8 *inline_data;
                Size        inline_bytes;

                root = pg_tre_posting_build_finish(current_builder,
                                                   &inline_data,
                                                   &inline_bytes);
                pg_tre_posting_build_free(current_builder);

                if (n_accums >= accums_alloced)
                {
                    accums_alloced *= 2;
                    accums = (PostingAccum *)
                        repalloc(accums, accums_alloced * sizeof(PostingAccum));
                }
                accums[n_accums].trigram_hash = current_hash;
                accums[n_accums].root = root;
                accums[n_accums].inline_data = inline_data;
                accums[n_accums].inline_bytes = inline_bytes;
                n_accums++;
            }
        }

        if (positions_buf)
            pfree(positions_buf);
    }

    ereport(NOTICE,
            (errmsg("pg_tre: built %d posting trees", n_accums)));

    /* Step 6: bulk-load upper tree from the posting list. */
    iter_state.accums = accums;
    iter_state.n_accums = n_accums;
    iter_state.current = 0;

    root_upper = pg_tre_upper_bulkload(index, upper_iter, &iter_state);

    /* Step 6.5: bulk-load range tier from postings (Phase 5). */
    {
        BlockNumber root_range = InvalidBlockNumber;

        if (pg_tre_tuple_bloom_enable)
        {
            /* Reset iterator and build range tree. */
            iter_state.current = 0;
            root_range = pg_tre_range_bulkload(index, upper_iter, &iter_state);
        }

        /* Step 7: update meta page with roots and stats. */
        pg_tre_meta_set_roots(index, root_upper, root_range,
                              (uint64) n_accums, (uint64) bstate.heap_tuples);
    }

    MemoryContextSwitchTo(oldcxt);
    MemoryContextDelete(bstate.tmpctx);

    /* Phase 5: free per-TID bloom tracking. */
    if (bstate.tid_blooms != NULL)
    {
        for (i = 0; i < bstate.n_tid_blooms; i++)
        {
            if (bstate.tid_blooms[i].bloom != NULL)
                pfree(bstate.tid_blooms[i].bloom);
        }
        pfree(bstate.tid_blooms);
    }

    pfree(bstate.entries);

    /* Return build result. */
    result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
    result->heap_tuples = bstate.heap_tuples;
    result->index_tuples = bstate.heap_tuples;  /* approximate */

    ereport(NOTICE,
            (errmsg("pg_tre: build complete, indexed %.0f heap tuples into %d trigrams",
                    bstate.heap_tuples, n_accums)));

    return result;
}

void
pg_tre_ambuildempty(Relation index)
{
    pg_tre_build_empty(index);
}
