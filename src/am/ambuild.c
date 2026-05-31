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

#include "varatt.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
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
#include "pg_tre/utf8.h"

/* In-memory sort entry: (trigram_hash, tid, position). */
typedef struct TrigramTidEntry
{
    uint64      trigram_hash;
    ItemPointerData tid;
    uint32      position;       /* byte offset in original text */
} TrigramTidEntry;

/*
 * Per-TID bloom tracking during build.
 *
 * Used as the element type for a simplehash-generated open-addressing
 * hash table keyed by packed_tid (an opaque uint64 derived from an
 * ItemPointerData via pg_tre_pack_tid).  See the SH_* macros below.
 *
 * The earlier implementation was a flat array with linear scans on
 * every (trigram,TID) emission and again at every post-sort flush;
 * for 100k-row builds this dominated CPU time at O(N^2) and made the
 * backend uncancellable.  Switching to simplehash collapses the
 * lookup to amortized O(1).
 */
typedef struct TidBloomEntry
{
    uint64      packed_tid;     /* SH_KEY */
    PgTreBloom *bloom;          /* palloc'd bloom filter */
    char        status;         /* required by simplehash */
} TidBloomEntry;

/* Generate the tid_bloom_hash type and tid_bloom_{create,lookup,insert,...} */
#define SH_PREFIX tid_bloom
#define SH_ELEMENT_TYPE TidBloomEntry
#define SH_KEY_TYPE uint64
#define SH_KEY packed_tid
#define SH_HASH_KEY(tb, key) hash_bytes((const unsigned char *) &(key), sizeof(uint64))
#define SH_EQUAL(tb, a, b) ((a) == (b))
#define SH_SCOPE static inline
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"

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
    tid_bloom_hash *tid_blooms;

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
 *
 * Backed by a simplehash-generated open-addressing hash table keyed by
 * packed_tid; lookup and insert are amortized O(1).  When a new entry
 * is created, the bloom filter is allocated in CurrentMemoryContext so
 * it lives for the duration of the build.
 */
static PgTreBloom *
find_or_create_tid_bloom(BuildState *bstate, ItemPointer tid)
{
    uint64 packed = pg_tre_pack_tid(tid);
    TidBloomEntry *entry;
    bool        found;

    if (!pg_tre_tuple_bloom_enable)
        return NULL;

    entry = tid_bloom_lookup(bstate->tid_blooms, packed);
    if (entry != NULL)
        return entry->bloom;

    entry = tid_bloom_insert(bstate->tid_blooms, packed, &found);
    if (!found)
    {
        Size bloom_size = pg_tre_bloom_size_bytes(pg_tre_bloom_tuple_bits);
        PgTreBloom *bloom = (PgTreBloom *) palloc(bloom_size);

        pg_tre_bloom_init(bloom, pg_tre_bloom_tuple_bits, 5 /* k */);
        entry->bloom = bloom;
    }
    return entry->bloom;
}

/*
 * Extract trigrams from a text datum and insert them into the build state.
 * Phase 3.5: extract codepoint trigrams using UTF-8 streaming.
 * For ASCII text, this is equivalent to byte trigrams (no regression).
 * Phase 5: also populate per-tuple bloom filter and record positions.
 */
static void
extract_trigrams(BuildState *bstate, Datum value, bool isnull, ItemPointer tid)
{
    text       *txt;
    char       *str;
    int         len;
    PgTreCpStream stream;
    int32       ring[3];   /* ring buffer of last 3 codepoints */
    int         ring_pos[3]; /* byte positions where each codepoint starts */
    int         ring_n = 0;/* how many codepoints we've seen so far */
    int32       cp;
    int         cp_start;  /* byte offset where current codepoint starts */
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

    /* Initialize codepoint stream. */
    pg_tre_cpstream_init(&stream, str, len);

    /*
     * Streaming loop: fill a 3-element ring buffer, emit trigrams from
     * each consecutive triple of codepoints.
     *
     * Positions are byte offsets (for TRE's byte-based recheck), not
     * codepoint indices. We track the byte position of the start of each
     * codepoint in the ring buffer so trigram positions are accurate.
     */
    while (true)
    {
        cp_start = pg_tre_cpstream_pos(&stream);
        cp = pg_tre_cpstream_next(&stream);
        if (cp < 0)
            break;

        /* Shift the ring buffer. */
        if (ring_n >= 3)
        {
            ring[0] = ring[1];
            ring[1] = ring[2];
            ring[2] = cp;
            ring_pos[0] = ring_pos[1];
            ring_pos[1] = ring_pos[2];
            ring_pos[2] = cp_start;
        }
        else
        {
            ring[ring_n] = cp;
            ring_pos[ring_n] = cp_start;
            ring_n++;
        }

        /* Emit a trigram once we have 3 codepoints. */
        if (ring_n == 3)
        {
            uint64  trigram_hash;
            TrigramTidEntry *entry;

            trigram_hash = pg_tre_hash_trigram_cp(ring);

            /* Grow array if needed.  Uses _huge variants because
             * a 1M-row corpus emits ~50M trigram entries (~1.2 GB),
             * exceeding palloc's 1 GB MaxAllocSize cap. */
            if (bstate->n_entries >= bstate->entries_alloced)
            {
                bstate->entries_alloced *= 2;
                bstate->entries = (TrigramTidEntry *)
                    repalloc_huge(bstate->entries,
                             bstate->entries_alloced * sizeof(TrigramTidEntry));
            }

            entry = &bstate->entries[bstate->n_entries++];
            entry->trigram_hash = trigram_hash;
            entry->tid = *tid;
            /* Position is the byte offset where the first codepoint starts. */
            entry->position = (uint32) ring_pos[0];

            /* Phase 5: add trigram to the per-tuple bloom. */
            if (bloom != NULL)
                pg_tre_bloom_add_trigram(bloom, trigram_hash);
        }
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
     * Heap scan can run for minutes on large relations; without an
     * interrupt check the backend ignores pg_cancel_backend /
     * pg_terminate_backend until the entire scan completes.  Same
     * defense as the post-sort loop below.
     */
    CHECK_FOR_INTERRUPTS();

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
    int         n_skipped_trigrams;  /* dropped via min_trigram_freq */
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
        MemoryContextAllocHuge(CurrentMemoryContext,
                               bstate.entries_alloced * sizeof(TrigramTidEntry));
    bstate.n_entries = 0;

    /* Phase 5: initialize per-TID bloom hash table. */
    bstate.tid_blooms = tid_bloom_create(CurrentMemoryContext, 1024, NULL);

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
    n_skipped_trigrams = 0;

    current_hash = 0;
    current_builder = NULL;

    /* Phase 5: accumulate positions per (trigram, TID) pair. */
    {
        uint32 *positions_buf = NULL;
        int positions_alloced = 0;
        int n_positions = 0;
        uint64 current_tid_packed = UINT64_MAX;   /* sentinel: no TID yet */

        for (i = 0; i < bstate.n_entries; i++)
        {
            /*
             * Allow CREATE INDEX [CONCURRENTLY] to be cancelled.
             * On large heaps n_entries can be in the tens of millions
             * and this loop runs for minutes; without an interrupt
             * check, pg_cancel_backend / pg_terminate_backend are
             * silently ignored.  Same root cause as the autovacuum
             * runaway fix in pending.c.
             */
            CHECK_FOR_INTERRUPTS();

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
                    if (current_tid_packed != UINT64_MAX)
                    {
                        ItemPointerData tid;
                        PgTreBloom *bloom = NULL;
                        uint8 *bloom_bits = NULL;

                        pg_tre_unpack_tid(current_tid_packed, &tid);

                        /* Look up the TID's bloom. */
                        if (pg_tre_tuple_bloom_enable)
                        {
                            TidBloomEntry *be =
                                tid_bloom_lookup(bstate.tid_blooms,
                                                 current_tid_packed);
                            if (be != NULL)
                                bloom = be->bloom;
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
                        int         n_tids;

                        n_tids = pg_tre_posting_build_n_tids(
                                     current_builder);

                        /*
                         * Cardinality-aware build (1.2.1+):
                         * skip persisting posting trees for
                         * trigrams that appear in fewer than
                         * pg_tre.min_trigram_freq rows.  These
                         * trigrams aren't useful candidate
                         * filters; recheck handles correctness.
                         */
                        if (pg_tre_min_trigram_freq > 1 &&
                            n_tids < pg_tre_min_trigram_freq)
                        {
                            pg_tre_posting_build_free(current_builder);
                            n_skipped_trigrams++;
                        }
                        else
                        {
                            root = pg_tre_posting_build_finish(
                                       current_builder,
                                       &inline_data,
                                       &inline_bytes);
                            pg_tre_posting_build_free(current_builder);

                            /* Record the completed posting. */
                            if (n_accums >= accums_alloced)
                            {
                                accums_alloced *= 2;
                                accums = (PostingAccum *)
                                    repalloc(accums,
                                             accums_alloced * sizeof(PostingAccum));
                            }
                            accums[n_accums].trigram_hash = current_hash;
                            accums[n_accums].root = root;
                            accums[n_accums].inline_data = inline_data;
                            accums[n_accums].inline_bytes = inline_bytes;
                            n_accums++;
                        }
                    }
                }

                /* Start a new posting tree. */
                current_hash = entry->trigram_hash;
                current_builder = pg_tre_posting_build_begin(
                                      index, current_hash,
                                      pg_tre_tuple_bloom_enable /* with_payload */);
                n_positions = 0;
                current_tid_packed = UINT64_MAX;  /* reset for new trigram */
            }

            if (new_tid && !new_trigram)
            {
                /* Flush accumulated positions for the previous TID. */
                if (current_tid_packed != UINT64_MAX)
                {
                    ItemPointerData tid;
                    PgTreBloom *bloom = NULL;
                    uint8 *bloom_bits = NULL;

                    pg_tre_unpack_tid(current_tid_packed, &tid);

                    /* Look up the TID's bloom. */
                    if (pg_tre_tuple_bloom_enable)
                    {
                        TidBloomEntry *be =
                            tid_bloom_lookup(bstate.tid_blooms,
                                             current_tid_packed);
                        if (be != NULL)
                            bloom = be->bloom;
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
                    int new_cap = (positions_alloced == 0) ? 16 :
                                  positions_alloced * 2;
                    positions_buf = (uint32 *)
                        (positions_buf == NULL
                         ? palloc(new_cap * sizeof(uint32))
                         : repalloc(positions_buf, new_cap * sizeof(uint32)));
                    positions_alloced = new_cap;
                }
                positions_buf[n_positions++] = entry->position;
            }
        }

        /* Finish the last posting tree. */
        if (current_builder != NULL)
        {
            /* Add the last accumulated TID. */
            if (current_tid_packed != UINT64_MAX)
            {
                ItemPointerData tid;
                PgTreBloom *bloom = NULL;
                uint8 *bloom_bits = NULL;

                pg_tre_unpack_tid(current_tid_packed, &tid);

                /* Look up the TID's bloom. */
                if (pg_tre_tuple_bloom_enable)
                {
                    TidBloomEntry *be =
                        tid_bloom_lookup(bstate.tid_blooms,
                                         current_tid_packed);
                    if (be != NULL)
                        bloom = be->bloom;
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
                int         n_tids;

                n_tids = pg_tre_posting_build_n_tids(current_builder);

                if (pg_tre_min_trigram_freq > 1 &&
                    n_tids < pg_tre_min_trigram_freq)
                {
                    pg_tre_posting_build_free(current_builder);
                    n_skipped_trigrams++;
                }
                else
                {
                    root = pg_tre_posting_build_finish(current_builder,
                                                       &inline_data,
                                                       &inline_bytes);
                    pg_tre_posting_build_free(current_builder);

                    if (n_accums >= accums_alloced)
                    {
                        accums_alloced *= 2;
                        accums = (PostingAccum *)
                            repalloc(accums,
                                     accums_alloced * sizeof(PostingAccum));
                    }
                    accums[n_accums].trigram_hash = current_hash;
                    accums[n_accums].root = root;
                    accums[n_accums].inline_data = inline_data;
                    accums[n_accums].inline_bytes = inline_bytes;
                    n_accums++;
                }
            }
        }

        if (positions_buf)
            pfree(positions_buf);
    }

    if (n_skipped_trigrams > 0)
        ereport(NOTICE,
                (errmsg("pg_tre: built %d posting trees "
                        "(%d trigrams skipped: below pg_tre.min_trigram_freq=%d)",
                        n_accums, n_skipped_trigrams,
                        pg_tre_min_trigram_freq)));
    else
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

    /* Phase 5: free per-TID bloom hash table and the blooms it owns. */
    if (bstate.tid_blooms != NULL)
    {
        tid_bloom_iterator iter;
        TidBloomEntry *entry;

        tid_bloom_start_iterate(bstate.tid_blooms, &iter);
        while ((entry = tid_bloom_iterate(bstate.tid_blooms, &iter)) != NULL)
        {
            if (entry->bloom != NULL)
                pfree(entry->bloom);
        }
        tid_bloom_destroy(bstate.tid_blooms);
        bstate.tid_blooms = NULL;
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
    /*
     * ambuildempty populates the INIT fork of an UNLOGGED index.
     * The init fork is the WAL-logged template that gets copied
     * to the main fork during crash recovery.
     *
     * Calling pg_tre_build_empty here (which extends MAIN_FORKNUM)
     * tripped the metabuf-block-number assertion because the main
     * fork's block 0 was already allocated by ambuild.  Discovered
     * by test/scripts/wal_audit.sh.
     */
    pg_tre_build_empty_fork(index, INIT_FORKNUM);
}
