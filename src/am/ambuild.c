/*
 * src/am/ambuild.c - index build phase.
 *
 * Phase 2: tuplesort-backed bulk build.
 *
 * Algorithm:
 *   1. Initialize empty meta page
 *   2. Scan heap, extract byte trigrams from indexed text column
 *   3. Hash each trigram to uint64, emit (hash, TID) into tuplesort
 *   4. Sort by (hash, TID)
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
#include "utils/tuplesort.h"

#include "pg_tre/amapi.h"
#include "pg_tre/hash.h"
#include "pg_tre/meta.h"
#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/posting.h"
#include "pg_tre/upper.h"

/* Tuplesort record: (trigram_hash, tid). */
typedef struct TrigramTidEntry
{
    uint64      trigram_hash;
    ItemPointerData tid;
} TrigramTidEntry;

/* Callback context for heap scan. */
typedef struct BuildState
{
    Relation    heap;
    Relation    index;
    IndexInfo  *indexInfo;
    Tuplesortstate *sortstate;
    double      heap_tuples;
    MemoryContext tmpctx;
} BuildState;

/* Accumulator for posting entries during sort readout. */
typedef struct PostingAccum
{
    uint64      trigram_hash;
    PgTrePostingBuilder *builder;
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
 * Comparison function for tuplesort: order by (trigram_hash, tid).
 */
static int
compare_trigram_tid(const SortTuple *a, const SortTuple *b, Tuplesortstate *state)
{
    TrigramTidEntry *ea = (TrigramTidEntry *) a->tuple;
    TrigramTidEntry *eb = (TrigramTidEntry *) b->tuple;

    if (ea->trigram_hash < eb->trigram_hash)
        return -1;
    if (ea->trigram_hash > eb->trigram_hash)
        return 1;

    /* Same hash: order by TID. */
    return ItemPointerCompare(&ea->tid, &eb->tid);
}

/*
 * Extract trigrams from a text datum and insert them into tuplesort.
 */
static void
extract_trigrams(BuildState *bstate, Datum value, bool isnull, ItemPointer tid)
{
    text       *txt;
    char       *str;
    int         len;
    int         i;
    TrigramTidEntry entry;

    if (isnull)
        return;

    /* Detoast if needed. */
    txt = DatumGetTextPP(value);
    str = VARDATA_ANY(txt);
    len = VARSIZE_ANY_EXHDR(txt);

    /* Extract byte trigrams: every 3 consecutive bytes. */
    for (i = 0; i <= len - 3; i++)
    {
        uint8   trigram[3];
        trigram[0] = (uint8) str[i];
        trigram[1] = (uint8) str[i + 1];
        trigram[2] = (uint8) str[i + 2];

        entry.trigram_hash = pg_tre_hash_trigram(trigram);
        entry.tid = *tid;

        tuplesort_putbinary(bstate->sortstate, &entry, sizeof(entry));
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
    Tuplesortstate *sortstate;
    MemoryContext oldcxt;
    TrigramTidEntry *entry;
    bool        should_free;
    PostingAccum *accums;
    int         n_accums;
    int         accums_alloced;
    uint64      current_hash;
    PgTrePostingBuilder *current_builder;
    BlockNumber root_upper;
    UpperIterState iter_state;

    /* Phase 2 real build starts here. */

    /* Step 1: initialize empty meta page. */
    pg_tre_build_empty(index);

    /* Step 2: set up tuplesort for (trigram_hash, tid) pairs. */
    sortstate = tuplesort_begin_heap(
        NULL,                          /* no tupdesc for binary sort */
        0,                             /* no key columns (custom comparator) */
        NULL,                          /* no sort keys */
        maintenance_work_mem,
        NULL,                          /* no coordinate for parallel */
        0);                            /* no parallel workers */

    /* Override comparison function. */
    tuplesort_set_bound(sortstate, 0);

    bstate.heap = heap;
    bstate.index = index;
    bstate.indexInfo = indexInfo;
    bstate.sortstate = sortstate;
    bstate.heap_tuples = 0.0;
    bstate.tmpctx = AllocSetContextCreate(CurrentMemoryContext,
                                          "pg_tre build temp context",
                                          ALLOCSET_DEFAULT_SIZES);

    /* Step 3: scan heap and insert trigrams into tuplesort. */
    table_index_build_scan(heap, index, indexInfo, true, true,
                           build_callback, &bstate, NULL);

    /* Step 4: sort. */
    tuplesort_performsort(sortstate);

    /* Step 5: read sorted entries and build posting trees. */
    oldcxt = MemoryContextSwitchTo(bstate.tmpctx);

    accums_alloced = 1024;
    accums = (PostingAccum *) palloc(accums_alloced * sizeof(PostingAccum));
    n_accums = 0;

    current_hash = 0;
    current_builder = NULL;

    while ((entry = (TrigramTidEntry *) tuplesort_getbinary(sortstate, true,
                                                            &should_free)) != NULL)
    {
        if (current_builder == NULL || entry->trigram_hash != current_hash)
        {
            /* Finish the previous trigram's posting tree. */
            if (current_builder != NULL)
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

            /* Start a new posting tree for this trigram. */
            current_hash = entry->trigram_hash;
            current_builder = pg_tre_posting_build_begin(index, current_hash,
                                                         false /* no payload */);
        }

        /* Add TID to the current posting builder. */
        pg_tre_posting_build_add(current_builder, &entry->tid,
                                 NULL, 0, NULL);

        if (should_free)
            pfree(entry);
    }

    /* Finish the last posting tree. */
    if (current_builder != NULL)
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

    tuplesort_end(sortstate);

    /* Step 6: bulk-load upper tree from the posting list. */
    iter_state.accums = accums;
    iter_state.n_accums = n_accums;
    iter_state.current = 0;

    root_upper = pg_tre_upper_bulkload(index, upper_iter, &iter_state);

    /* Step 7: update meta page with roots and stats. */
    pg_tre_meta_set_roots(index, root_upper, InvalidBlockNumber,
                          (uint64) n_accums, (uint64) bstate.heap_tuples);

    MemoryContextSwitchTo(oldcxt);
    MemoryContextDelete(bstate.tmpctx);

    /* Return build result. */
    result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
    result->heap_tuples = bstate.heap_tuples;
    result->index_tuples = bstate.heap_tuples;  /* approximate */

    return result;
}

void
pg_tre_ambuildempty(Relation index)
{
    pg_tre_build_empty(index);
}
