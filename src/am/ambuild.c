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
#include "access/parallel.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "optimizer/optimizer.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "tcop/tcopprot.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"
#include "utils/wait_event.h"

#include "funcapi.h"
#include "access/relation.h"

#include "pg_tre/amapi.h"
#include "pg_tre/coalesced.h"
#include "pg_tre/hash.h"
#include "pg_tre/meta.h"
#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/posting.h"
#include "pg_tre/surf.h"
#include "pg_tre/surf_page.h"
#include "pg_tre/upper.h"
#include "pg_tre/utf8.h"

/* In-memory sort entry: (trigram_hash, tid, position).  Positions are
 * carried only to keep the historical sort order stable; the per-tuple
 * payload path that consumed them was removed in 3.0.0. */
typedef struct TrigramTidEntry
{
    uint64      trigram_hash;
    ItemPointerData tid;
    uint32      position;       /* byte offset in original text */
} TrigramTidEntry;

/* Callback context for heap scan. */
typedef struct BuildState
{
    Relation    heap;
    Relation    index;
    IndexInfo  *indexInfo;

    /*
     * Sort of (trigram_hash, tid, position) tuples via PostgreSQL's
     * tuplesort, so peak build memory is bounded by
     * maintenance_work_mem (disk-spilled) instead of growing with the
     * total number of trigram emissions.  Each tuple is encoded as a
     * fixed 20-byte big-endian bytea whose memcmp order equals the
     * historical (hash, packed_tid, position) order.  See
     * encode_entry()/decode_entry().
     */
    Tuplesortstate *sortstate;
    int64       n_emitted;       /* total tuples put into the sort */

    double      heap_tuples;
    MemoryContext tmpctx;

    /*
     * Parallel build coordination.  NULL/absent for a serial build.
     * btshared points at the DSM-resident shared state; sharedsort is
     * the tuplesort coordination object all participants attach to.
     */
    struct PgTreShared *pgtshared;
    Sharedsort         *sharedsort;
} BuildState;

/*
 * Status record for a parallel index build, resident in the DSM segment
 * shared by the leader and all workers.  Mirrors nbtree's BTShared.
 */
typedef struct PgTreShared
{
    /* Immutable state set up by the leader before launching workers. */
    Oid         heaprelid;
    Oid         indexrelid;
    bool        isconcurrent;
    int         scantuplesortstates;

    /*
     * mutex protects the mutable fields below and coordinates worker
     * completion (workersdonecv).
     */
    slock_t     mutex;
    ConditionVariable workersdonecv;

    /* Mutable state, protected by mutex. */
    int         nparticipantsdone;
    double      reltuples;       /* summed heap tuples scanned */
    double      indtuples;       /* summed trigram tuples emitted */

    /*
     * ParallelTableScanDescData follows immediately (variable length);
     * fetch via a computed offset, not embedded, so the struct stays
     * fixed-size for the DSM key layout.
     */
} PgTreShared;

/* DSM keys for the parallel build TOC. */
#define PG_TRE_BUILD_KEY_SHARED         UINT64CONST(0xB000000000000001)
#define PG_TRE_BUILD_KEY_TUPLESORT      UINT64CONST(0xB000000000000002)
#define PG_TRE_BUILD_KEY_QUERY_TEXT     UINT64CONST(0xB000000000000003)
#define PG_TRE_BUILD_KEY_WAL_USAGE      UINT64CONST(0xB000000000000004)
#define PG_TRE_BUILD_KEY_BUFFER_USAGE   UINT64CONST(0xB000000000000005)

/*
 * Leader-private state for a parallel build: the ParallelContext plus
 * pointers into the DSM segment.  Mirrors nbtree's BTLeader.
 */
typedef struct PgTreLeader
{
    ParallelContext *pcxt;
    int              nparticipanttuplesorts;
    PgTreShared     *pgtshared;
    Sharedsort      *sharedsort;
    Snapshot         snapshot;
    WalUsage        *walusage;
    BufferUsage     *bufferusage;
} PgTreLeader;

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
 * Tuplesort entry encoding.
 *
 * Each (trigram_hash, tid, position) tuple is encoded as a fixed
 * 20-byte big-endian byte string and sorted as a `bytea` Datum.
 * Because every encoded value is exactly 20 bytes, bytea's
 * memcmp-based ordering is identical to the historical
 * (trigram_hash ASC, ItemPointerCompare(tid) ASC, position ASC)
 * comparator:
 *
 *   bytes  0..7  : trigram_hash          (uint64, big-endian)
 *   bytes  8..15 : pg_tre_pack_tid(tid)  (uint64, big-endian)
 *   bytes 16..23 : trigram_key           (uint64, big-endian; the
 *                  order-preserving pg_tre_trigram_key_cp key, used to
 *                  build the v10 SuRF filter -- carried through the sort
 *                  so the merged distinct-trigram stream yields keys for
 *                  both serial and parallel builds.  Sorting by
 *                  (hash, tid) is unaffected: the trailing key only
 *                  breaks ties between identical (hash,tid) pairs, which
 *                  never occur, so grouping by hash is unchanged.)
 *
 * pg_tre_pack_tid() is (block << 16) | offset, so the numeric order
 * of the packed value equals ItemPointerCompare order, and big-endian
 * bytes preserve that under memcmp.
 */
#define PG_TRE_SORTKEY_LEN 24

/*
 * Realistic temp-disk cost of one emitted trigram tuple inside
 * tuplesort, in bytes.  The encoded key is 24 bytes (4-byte bytea
 * header + 20-byte sortkey), but tuplesort_begin_datum wraps every
 * by-reference Datum in a SortTuple plus a MinimalTuple and rounds
 * up, so the on-tape/in-memory footprint is substantially larger.
 * A production user measured ~21 GB of build temp for ~tens of MB
 * of indexed text -- consistent with ~64 bytes per emitted tuple.
 * build_max_entries_mb uses THIS figure (not the bare 24-byte key)
 * so its ceiling tracks real temp-disk consumption rather than
 * under-counting it ~2.5x.  See LIMITATIONS.md for the sizing model.
 */
#define PG_TRE_SORT_TUPLE_TEMP_BYTES 64

/* qsort comparator for the SuRF key array (ascending uint64). */
static int
cmp_uint64(const void *a, const void *b)
{
    uint64      x = *(const uint64 *) a;
    uint64      y = *(const uint64 *) b;

    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static inline void
encode_entry(uint8 *buf, uint64 trigram_hash, uint64 packed_tid,
             uint64 trigram_key)
{
    buf[0]  = (uint8) (trigram_hash >> 56);
    buf[1]  = (uint8) (trigram_hash >> 48);
    buf[2]  = (uint8) (trigram_hash >> 40);
    buf[3]  = (uint8) (trigram_hash >> 32);
    buf[4]  = (uint8) (trigram_hash >> 24);
    buf[5]  = (uint8) (trigram_hash >> 16);
    buf[6]  = (uint8) (trigram_hash >> 8);
    buf[7]  = (uint8) (trigram_hash);
    buf[8]  = (uint8) (packed_tid >> 56);
    buf[9]  = (uint8) (packed_tid >> 48);
    buf[10] = (uint8) (packed_tid >> 40);
    buf[11] = (uint8) (packed_tid >> 32);
    buf[12] = (uint8) (packed_tid >> 24);
    buf[13] = (uint8) (packed_tid >> 16);
    buf[14] = (uint8) (packed_tid >> 8);
    buf[15] = (uint8) (packed_tid);
    buf[16] = (uint8) (trigram_key >> 56);
    buf[17] = (uint8) (trigram_key >> 48);
    buf[18] = (uint8) (trigram_key >> 40);
    buf[19] = (uint8) (trigram_key >> 32);
    buf[20] = (uint8) (trigram_key >> 24);
    buf[21] = (uint8) (trigram_key >> 16);
    buf[22] = (uint8) (trigram_key >> 8);
    buf[23] = (uint8) (trigram_key);
}

static inline void
decode_entry(const uint8 *buf, uint64 *trigram_hash, uint64 *packed_tid,
             uint64 *trigram_key)
{
    *trigram_hash =
          ((uint64) buf[0]  << 56) | ((uint64) buf[1]  << 48)
        | ((uint64) buf[2]  << 40) | ((uint64) buf[3]  << 32)
        | ((uint64) buf[4]  << 24) | ((uint64) buf[5]  << 16)
        | ((uint64) buf[6]  << 8)  | ((uint64) buf[7]);
    *packed_tid =
          ((uint64) buf[8]  << 56) | ((uint64) buf[9]  << 48)
        | ((uint64) buf[10] << 40) | ((uint64) buf[11] << 32)
        | ((uint64) buf[12] << 24) | ((uint64) buf[13] << 16)
        | ((uint64) buf[14] << 8)  | ((uint64) buf[15]);
    *trigram_key =
          ((uint64) buf[16] << 56) | ((uint64) buf[17] << 48)
        | ((uint64) buf[18] << 40) | ((uint64) buf[19] << 32)
        | ((uint64) buf[20] << 24) | ((uint64) buf[21] << 16)
        | ((uint64) buf[22] << 8)  | ((uint64) buf[23]);
}

/*
 * Extract trigrams from a text datum and insert them into the build state.
 * Phase 3.5: extract codepoint trigrams using UTF-8 streaming.
 * For ASCII text, this is equivalent to byte trigrams (no regression).
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
    /*
     * Per-row de-duplication of (trigram, tid) emissions.
     *
     * Natural text repeats trigrams heavily within a single row (a
     * message body mentions the same words many times).  The old
     * build emitted one sort tuple per trigram *occurrence* -- so a
     * row with trigram "abc" at 50 byte offsets produced 50 sort
     * tuples that all collapse to the same posting TID.  With
     * tuplesort's per-tuple overhead this exploded build temp disk
     * (a production user reported ~21 GB of temp for tens of MB of
     * indexed text).  We now emit each distinct (trigram, tid) once.
     * The posting set is what matters; duplicate occurrences carry no
     * extra information for the (payload-free, 3.0.0) posting tree.
     *
     * The set is a simple open-addressing table of the row's trigram
     * hashes, sized to the row length and reset per row.
     */
    uint64     *seen = NULL;
    uint32      seen_cap = 0;
    uint32      seen_mask = 0;
    uint32      seen_n = 0;
    bool        seen_zero = false;   /* whether trigram_hash==0 was emitted */

    if (isnull)
        return;

    /* Detoast if needed. */
    txt = DatumGetTextPP(value);
    str = VARDATA_ANY(txt);
    len = VARSIZE_ANY_EXHDR(txt);

    /*
     * Size the per-row de-dup set to the next power of two >= len
     * (an upper bound on distinct trigrams in the row), with a small
     * floor.  Open-addressing load factor stays <= 0.5 because we
     * never insert more than `len` distinct hashes into a table of
     * capacity >= len*2 ... we use len rounded up then doubled-ish:
     * cap = next_pow2(len + 8) gives headroom; if a pathological row
     * exceeds it we simply skip dedup for the overflow (still
     * correct, just less collapsing).
     */
    {
        uint32 want = (uint32) (len > 0 ? len : 1) + 8;
        uint32 cap = 16;
        while (cap < want)
            cap <<= 1;
        if (cap > (1u << 22))   /* clamp at 4M slots (32 MB) for huge rows */
            cap = (1u << 22);
        seen_cap = cap;
        seen_mask = cap - 1;
        seen = (uint64 *) palloc0(sizeof(uint64) * cap);
        seen_n = 0;
    }

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
            uint8   key[PG_TRE_SORTKEY_LEN];
            struct
            {
                int32 vl_len_;
                uint8 data[PG_TRE_SORTKEY_LEN];
            }       wrap;

            trigram_hash = pg_tre_hash_trigram_cp(ring);

            /*
             * Per-row de-dup: skip this emission if we have already
             * emitted (trigram_hash, tid) for the current row.  Open-
             * addressing probe; slot value 0 means empty.  A genuine
             * hash of 0 is handled by the seen_zero flag.  If the set
             * is full (pathological row beyond seen_cap), fall through
             * and emit (correct, just no collapsing).
             */
            if (seen != NULL)
            {
                if (trigram_hash == 0)
                {
                    if (seen_zero)
                        goto skip_emit;
                    seen_zero = true;
                }
                else if (seen_n < seen_cap)
                {
                    uint32 slot = (uint32) (trigram_hash * 0x9E3779B97F4A7C15ULL
                                            >> 40) & seen_mask;
                    bool found = false;
                    uint32 probes = 0;

                    while (seen[slot] != 0)
                    {
                        if (seen[slot] == trigram_hash)
                        {
                            found = true;
                            break;
                        }
                        slot = (slot + 1) & seen_mask;
                        if (++probes >= seen_cap)
                            break;   /* table full; emit without dedup */
                    }
                    if (found)
                        goto skip_emit;
                    if (seen[slot] == 0)
                    {
                        seen[slot] = trigram_hash;
                        seen_n++;
                    }
                }
            }

            /*
             * Bounded-build guard: the build is bounded in *memory* by
             * maintenance_work_mem (tuplesort spills to disk), but a
             * runaway emission count still consumes temp disk.  Keep
             * the 1.7.0 contract -- fail cleanly with a clear error
             * rather than filling the temp tablespace -- by capping the
             * total emission count at the same byte-equivalent ceiling.
             * pg_tre.build_max_entries_mb == 0 disables the guard.
             */
            bstate->n_emitted++;
            if (pg_tre_build_max_entries_mb > 0 &&
                (uint64) bstate->n_emitted * PG_TRE_SORT_TUPLE_TEMP_BYTES >
                    (uint64) pg_tre_build_max_entries_mb * 1024 * 1024)
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("pg_tre: index build exceeded "
                                "pg_tre.build_max_entries_mb (%d MB)",
                                pg_tre_build_max_entries_mb),
                         errdetail("The build emitted %lld trigram tuples "
                                   "(~%lld MB of build temp disk at ~%d "
                                   "bytes/tuple).",
                                   (long long) bstate->n_emitted,
                                   (long long) ((uint64) bstate->n_emitted
                                                * PG_TRE_SORT_TUPLE_TEMP_BYTES
                                                / (1024 * 1024)),
                                   PG_TRE_SORT_TUPLE_TEMP_BYTES),
                         errhint("Raise pg_tre.build_max_entries_mb on a "
                                 "host with enough RAM/temp space, raise "
                                 "pg_tre.min_trigram_freq, index a "
                                 "smaller/shorter column, or use pg_trgm "
                                 "+ tsvector for this workload.  See "
                                 "LIMITATIONS.md.")));

            /*
             * Encode (hash, packed_tid, position) as a fixed 20-byte
             * big-endian bytea and hand it to tuplesort, which copies
             * the by-reference Datum into its own storage / tape -- so
             * the stack buffer is safe to reuse for the next emission
             * (no per-emission palloc).
             */
            encode_entry(key, trigram_hash, pg_tre_pack_tid(tid),
                         pg_tre_trigram_key_cp(ring));
            SET_VARSIZE(&wrap, VARHDRSZ + PG_TRE_SORTKEY_LEN);
            memcpy(wrap.data, key, PG_TRE_SORTKEY_LEN);
            tuplesort_putdatum(bstate->sortstate,
                               PointerGetDatum(&wrap), false);

        skip_emit:
            ;   /* dedup landing pad: this (trigram,tid) already emitted */
        }
    }

    if (seen != NULL)
        pfree(seen);
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

/* ---------------------------------------------------------------------
 * Parallel build support.
 *
 * The parallelizable phase is the heap scan + trigram extraction + sort
 * ingestion.  All participants (leader + workers) feed one coordinated
 * tuplesort; after every participant finishes its portion, the leader
 * performs the final merge, then serially builds the posting trees,
 * upper tree, and range tier from the fully-sorted stream.  This mirrors
 * nbtree's _bt_begin_parallel / _bt_parallel_scan_and_sort split.
 * ------------------------------------------------------------------- */

/* Forward declarations. */
static void pgtre_scan_and_sort(BuildState *bstate, bool progress);
void pgtre_end_parallel(PgTreLeader *leader);

/*
 * Attempt to launch parallel workers for the build.  On success, fills in
 * bstate->pgtshared / bstate->sharedsort and returns a PgTreLeader; on
 * failure (couldn't get workers) returns NULL and the caller falls back
 * to a serial build.
 */
static PgTreLeader *
pgtre_begin_parallel(BuildState *bstate, bool isconcurrent, int request)
{
    ParallelContext *pcxt;
    PgTreShared     *pgtshared;
    Sharedsort      *sharedsort;
    PgTreLeader     *leader;
    Snapshot         snapshot;
    Size             estshared;
    Size             estsort;
    ParallelTableScanDesc pscan;
    WalUsage        *walusage;
    BufferUsage     *bufferusage;
    char            *sharedquery;
    int              querylen;
    int              scantuplesortstates;

    EnterParallelMode();
    Assert(request > 0);

    pcxt = CreateParallelContext("pg_tre", "pg_tre_parallel_build_main",
                                 request);

    /* leader participates as a worker too, per nbtree. */
    scantuplesortstates = request + 1;

    /*
     * Choose the scan snapshot up front (nbtree pattern): a non-concurrent
     * build uses SnapshotAny and does its own visibility filtering; a
     * CONCURRENTLY build takes a regular MVCC snapshot and indexes whatever
     * is visible to it.  The same snapshot must drive the DSM estimate (an
     * MVCC snapshot is serialized into the parallel scan descriptor and
     * costs space), the scan initialization, and the workers'
     * ii_Concurrent flag -- otherwise table_index_build_scan mis-derives
     * OldestXmin and trips an assertion in heapam.
     */
    if (!isconcurrent)
        snapshot = SnapshotAny;
    else
        snapshot = RegisterSnapshot(GetTransactionSnapshot());

    /*
     * Estimate DSM space: shared status + parallel scan descriptor.
     * The scan descriptor is placed at BUFFERALIGN(sizeof(PgTreShared))
     * (see the pscan pointer computation below and in the worker); the
     * estimate must use the same alignment so shm_toc_allocate reserves
     * enough -- getting this wrong overruns into the next chunk and
     * corrupts the shared spinlock (manifests as a "stuck spinlock"
     * PANIC in a worker).  Mirrors nbtree's _bt_parallel_estimate_shared.
     */
    estshared = add_size(BUFFERALIGN(sizeof(PgTreShared)),
                         table_parallelscan_estimate(bstate->heap,
                                                     snapshot));
    shm_toc_estimate_chunk(&pcxt->estimator, estshared);

    /* Tuplesort coordination object. */
    estsort = tuplesort_estimate_shared(scantuplesortstates);
    shm_toc_estimate_chunk(&pcxt->estimator, estsort);

    shm_toc_estimate_keys(&pcxt->estimator, 2);

    /* Query text for worker debug/instrumentation. */
    if (debug_query_string)
    {
        querylen = strlen(debug_query_string);
        shm_toc_estimate_chunk(&pcxt->estimator, querylen + 1);
        shm_toc_estimate_keys(&pcxt->estimator, 1);
    }
    else
        querylen = 0;

    /* WAL/buffer usage instrumentation slots. */
    shm_toc_estimate_chunk(&pcxt->estimator,
                           mul_size(sizeof(WalUsage), pcxt->nworkers));
    shm_toc_estimate_keys(&pcxt->estimator, 1);
    shm_toc_estimate_chunk(&pcxt->estimator,
                           mul_size(sizeof(BufferUsage), pcxt->nworkers));
    shm_toc_estimate_keys(&pcxt->estimator, 1);

    InitializeParallelDSM(pcxt);

    /* If no workers could actually be launched, fall back to serial. */
    if (pcxt->seg == NULL)
    {
        DestroyParallelContext(pcxt);
        ExitParallelMode();
        return NULL;
    }

    /* Set up the shared status record. */
    pgtshared = (PgTreShared *) shm_toc_allocate(pcxt->toc, estshared);
    pgtshared->heaprelid = RelationGetRelid(bstate->heap);
    pgtshared->indexrelid = RelationGetRelid(bstate->index);
    pgtshared->isconcurrent = isconcurrent;
    pgtshared->scantuplesortstates = scantuplesortstates;
    SpinLockInit(&pgtshared->mutex);
    ConditionVariableInit(&pgtshared->workersdonecv);
    pgtshared->nparticipantsdone = 0;
    pgtshared->reltuples = 0.0;
    pgtshared->indtuples = 0.0;

    /* Parallel heap scan descriptor lives immediately after the struct. */
    pscan = (ParallelTableScanDesc) ((char *) pgtshared + BUFFERALIGN(sizeof(PgTreShared)));
    table_parallelscan_initialize(bstate->heap, pscan, snapshot);

    shm_toc_insert(pcxt->toc, PG_TRE_BUILD_KEY_SHARED, pgtshared);

    /* Set up the shared tuplesort coordination object. */
    sharedsort = (Sharedsort *) shm_toc_allocate(pcxt->toc, estsort);
    tuplesort_initialize_shared(sharedsort, scantuplesortstates, pcxt->seg);
    shm_toc_insert(pcxt->toc, PG_TRE_BUILD_KEY_TUPLESORT, sharedsort);

    /* Store the query text. */
    if (debug_query_string)
    {
        sharedquery = (char *) shm_toc_allocate(pcxt->toc, querylen + 1);
        memcpy(sharedquery, debug_query_string, querylen + 1);
        shm_toc_insert(pcxt->toc, PG_TRE_BUILD_KEY_QUERY_TEXT, sharedquery);
    }

    /* Instrumentation. */
    walusage = shm_toc_allocate(pcxt->toc,
                                mul_size(sizeof(WalUsage), pcxt->nworkers));
    shm_toc_insert(pcxt->toc, PG_TRE_BUILD_KEY_WAL_USAGE, walusage);
    bufferusage = shm_toc_allocate(pcxt->toc,
                                   mul_size(sizeof(BufferUsage), pcxt->nworkers));
    shm_toc_insert(pcxt->toc, PG_TRE_BUILD_KEY_BUFFER_USAGE, bufferusage);

    LaunchParallelWorkers(pcxt);

    leader = (PgTreLeader *) palloc0(sizeof(PgTreLeader));
    leader->pcxt = pcxt;
    leader->nparticipanttuplesorts = pcxt->nworkers_launched + 1;
    leader->pgtshared = pgtshared;
    leader->sharedsort = sharedsort;
    leader->snapshot = snapshot;
    leader->walusage = walusage;
    leader->bufferusage = bufferusage;

    /* If no workers actually started, tear down and fall back to serial. */
    if (pcxt->nworkers_launched == 0)
    {
        pgtre_end_parallel(leader);
        return NULL;
    }

    bstate->pgtshared = pgtshared;
    bstate->sharedsort = sharedsort;

    return leader;
}

/*
 * Shut down a parallel build: wait for workers, accumulate their tuple
 * counts, and destroy the parallel context.
 */
void
pgtre_end_parallel(PgTreLeader *leader)
{
    int i;

    /* Shut the workers down and gather instrumentation. */
    WaitForParallelWorkersToFinish(leader->pcxt);

    for (i = 0; i < leader->pcxt->nworkers_launched; i++)
        InstrAccumParallelQuery(&leader->bufferusage[i], &leader->walusage[i]);

    if (IsMVCCSnapshot(leader->snapshot))
        UnregisterSnapshot(leader->snapshot);

    DestroyParallelContext(leader->pcxt);
    ExitParallelMode();
}

/*
 * Wait for all participants to finish their scan+sort portion and return
 * the total number of heap tuples scanned across all participants.  Also
 * reports the summed emitted-trigram count via *indtuples.
 */
static double
pgtre_parallel_heapscan(PgTreLeader *leader, double *indtuples)
{
    PgTreShared *pgtshared = leader->pgtshared;
    int          nparticipants = leader->nparticipanttuplesorts;
    double       reltuples = 0.0;

    for (;;)
    {
        SpinLockAcquire(&pgtshared->mutex);
        if (pgtshared->nparticipantsdone == nparticipants)
        {
            reltuples = pgtshared->reltuples;
            *indtuples = pgtshared->indtuples;
            SpinLockRelease(&pgtshared->mutex);
            break;
        }
        SpinLockRelease(&pgtshared->mutex);

        ConditionVariableSleep(&pgtshared->workersdonecv,
                               WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
    }

    ConditionVariableCancelSleep();
    return reltuples;
}

/*
 * Scan the (portion of the) heap assigned to this participant, extract
 * trigrams, and feed them to the coordinated tuplesort.  Used by both the
 * leader-as-worker path and the background workers.  Does NOT call
 * tuplesort_performsort (each participant sorts its own run; the leader
 * does the final cross-run merge on readout).
 */
static void
pgtre_scan_and_sort(BuildState *bstate, bool progress)
{
    if (bstate->pgtshared != NULL)
    {
        /* Parallel: scan our slice of the shared parallel scan. */
        ParallelTableScanDesc pscan =
            (ParallelTableScanDesc) ((char *) bstate->pgtshared +
                                     BUFFERALIGN(sizeof(PgTreShared)));
        TableScanDesc scan = table_beginscan_parallel(bstate->heap, pscan);

        bstate->heap_tuples =
            table_index_build_scan(bstate->heap, bstate->index,
                                   bstate->indexInfo, true, progress,
                                   build_callback, bstate, scan);
    }
    else
    {
        /* Serial. */
        bstate->heap_tuples =
            table_index_build_scan(bstate->heap, bstate->index,
                                   bstate->indexInfo, true, progress,
                                   build_callback, bstate, NULL);
    }
}

/*
 * Parallel worker entry point.  Attaches to the DSM segment, opens the
 * heap+index, feeds its heap slice into the coordinated tuplesort, and
 * reports its tuple counts back to the leader.
 */
PGDLLEXPORT void
pg_tre_parallel_build_main(dsm_segment *seg, shm_toc *toc)
{
    PgTreShared    *pgtshared;
    Sharedsort     *sharedsort;
    BuildState      wstate;
    Relation        heapRel;
    Relation        indexRel;
    LOCKMODE        heapLockmode;
    LOCKMODE        indexLockmode;
    WalUsage       *walusage;
    BufferUsage    *bufferusage;
    char           *sharedquery;
    TypeCacheEntry *tc;
    int             sortmem;

    /* Enable instrumentation for this worker. */
    InstrStartParallelQuery();

    pgtshared = (PgTreShared *) shm_toc_lookup(toc, PG_TRE_BUILD_KEY_SHARED,
                                               false);
    sharedsort = (Sharedsort *) shm_toc_lookup(toc, PG_TRE_BUILD_KEY_TUPLESORT,
                                               false);

    /* Optional query text for pg_stat_activity. */
    sharedquery = shm_toc_lookup(toc, PG_TRE_BUILD_KEY_QUERY_TEXT, true);
    if (sharedquery)
        debug_query_string = sharedquery;

    if (pgtshared->isconcurrent)
    {
        heapLockmode = ShareUpdateExclusiveLock;
        indexLockmode = RowExclusiveLock;
    }
    else
    {
        heapLockmode = ShareLock;
        indexLockmode = AccessExclusiveLock;
    }

    heapRel = table_open(pgtshared->heaprelid, heapLockmode);
    indexRel = index_open(pgtshared->indexrelid, indexLockmode);

    memset(&wstate, 0, sizeof(wstate));
    wstate.heap = heapRel;
    wstate.index = indexRel;
    wstate.indexInfo = BuildIndexInfo(indexRel);
    wstate.indexInfo->ii_Concurrent = pgtshared->isconcurrent;
    wstate.n_emitted = 0;
    wstate.pgtshared = pgtshared;
    wstate.sharedsort = sharedsort;
    wstate.tmpctx = AllocSetContextCreate(CurrentMemoryContext,
                                          "pg_tre parallel worker temp",
                                          ALLOCSET_DEFAULT_SIZES);

    /* Attach to the coordinated tuplesort as a worker participant. */
    tc = lookup_type_cache(BYTEAOID, TYPECACHE_LT_OPR);
    if (!OidIsValid(tc->lt_opr))
        elog(ERROR, "pg_tre: no btree \"<\" operator for bytea");

    tuplesort_attach_shared(sharedsort, seg);
    sortmem = maintenance_work_mem / pgtshared->scantuplesortstates;

    {
        SortCoordinate coordinate = palloc0(sizeof(SortCoordinateData));

        coordinate->isWorker = true;
        coordinate->nParticipants = -1;
        coordinate->sharedsort = sharedsort;

        wstate.sortstate = tuplesort_begin_datum(BYTEAOID, tc->lt_opr,
                                                 InvalidOid, false, sortmem,
                                                 coordinate, TUPLESORT_NONE);
    }

    /* Scan our heap slice, feeding the sort. */
    pgtre_scan_and_sort(&wstate, false);
    tuplesort_performsort(wstate.sortstate);

    /* Report our tuple counts to the leader. */
    SpinLockAcquire(&pgtshared->mutex);
    pgtshared->nparticipantsdone++;
    pgtshared->reltuples += wstate.heap_tuples;
    pgtshared->indtuples += (double) wstate.n_emitted;
    SpinLockRelease(&pgtshared->mutex);

    ConditionVariableSignal(&pgtshared->workersdonecv);

    tuplesort_end(wstate.sortstate);

    /* Publish instrumentation into the leader-visible arrays. */
    walusage = shm_toc_lookup(toc, PG_TRE_BUILD_KEY_WAL_USAGE, false);
    bufferusage = shm_toc_lookup(toc, PG_TRE_BUILD_KEY_BUFFER_USAGE, false);
    InstrEndParallelQuery(&bufferusage[ParallelWorkerNumber],
                          &walusage[ParallelWorkerNumber]);

    MemoryContextDelete(wstate.tmpctx);
    index_close(indexRel, indexLockmode);
    table_close(heapRel, heapLockmode);
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
    PgTreCoalescedWriter *coalesce_writer = NULL;
    PgTreLeader *build_leader = NULL;

    /* v10 SuRF: distinct order-preserving trigram keys, collected in
     * ascending order from the merged sort stream (keys arrive grouped by
     * hash, NOT by key, so we sort the collected array before building). */
    uint64     *surf_keys = NULL;
    uint32      surf_n = 0;
    uint32      surf_cap = 0;

    /* Phase 2 real build starts here. */

    /* Step 1: initialize empty meta page. */
    pg_tre_build_empty(index);

    /* Step 2: set up the disk-spillable sort and bloom tracking. */
    bstate.heap = heap;
    bstate.index = index;
    bstate.indexInfo = indexInfo;
    bstate.n_emitted = 0;
    bstate.pgtshared = NULL;
    bstate.sharedsort = NULL;
    bstate.heap_tuples = 0.0;
    bstate.tmpctx = AllocSetContextCreate(CurrentMemoryContext,
                                          "pg_tre build temp context",
                                          ALLOCSET_DEFAULT_SIZES);

    {
        PgTreLeader *leader = NULL;
        TypeCacheEntry *tc = lookup_type_cache(BYTEAOID, TYPECACHE_LT_OPR);
        int         sortmem;

        if (!OidIsValid(tc->lt_opr))
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("pg_tre: no btree \"<\" operator for bytea")));

        /*
         * Try to launch a parallel build if the planner requested workers
         * (indexInfo->ii_ParallelWorkers, set by plan_create_index_workers
         * via amcanbuildparallel=true) AND the experimental GUC is on.  If
         * workers can't be obtained we transparently fall back to a serial
         * build.
         */
        if (pg_tre_enable_parallel_build && indexInfo->ii_ParallelWorkers > 0)
            leader = pgtre_begin_parallel(&bstate,
                                          indexInfo->ii_Concurrent,
                                          indexInfo->ii_ParallelWorkers);

        if (leader != NULL)
        {
            /*
             * Parallel build.  Two tuplesort roles for the leader:
             *
             *  1. bstate.sortstate is the *leader-role* sort
             *     (isWorker=false, nParticipants=N): it is NOT fed tuples,
             *     only tuplesort_performsort()'d to merge every worker's
             *     partial run and read out below.
             *
             *  2. The leader also participates as a *worker* by scanning
             *     its own heap slice into a separate worker-role sort
             *     (isWorker=true) that feeds the shared sort, exactly like
             *     a background worker (mirrors nbtree's
             *     _bt_leader_participate_as_worker).
             */
            build_leader = leader;
            {
                SortCoordinate coordinate = palloc0(sizeof(SortCoordinateData));

                coordinate->isWorker = false;
                coordinate->nParticipants = leader->nparticipanttuplesorts;
                coordinate->sharedsort = bstate.sharedsort;

                bstate.sortstate = tuplesort_begin_datum(BYTEAOID, tc->lt_opr,
                                                         InvalidOid, false,
                                                         maintenance_work_mem,
                                                         coordinate,
                                                         TUPLESORT_NONE);
            }

            /* Leader participates as a worker: scan a slice, feed shared sort. */
            {
                BuildState  wstate;
                SortCoordinate wcoord = palloc0(sizeof(SortCoordinateData));

                memset(&wstate, 0, sizeof(wstate));
                wstate.heap = heap;
                wstate.index = index;
                wstate.indexInfo = BuildIndexInfo(index);
                wstate.indexInfo->ii_Concurrent = indexInfo->ii_Concurrent;
                wstate.n_emitted = 0;
                wstate.pgtshared = bstate.pgtshared;
                wstate.sharedsort = bstate.sharedsort;
                wstate.heap_tuples = 0.0;
                wstate.tmpctx = AllocSetContextCreate(CurrentMemoryContext,
                                                      "pg_tre leader-worker temp",
                                                      ALLOCSET_DEFAULT_SIZES);

                sortmem = maintenance_work_mem / leader->nparticipanttuplesorts;
                wcoord->isWorker = true;
                wcoord->nParticipants = -1;
                wcoord->sharedsort = bstate.sharedsort;
                wstate.sortstate = tuplesort_begin_datum(BYTEAOID, tc->lt_opr,
                                                         InvalidOid, false,
                                                         sortmem, wcoord,
                                                         TUPLESORT_NONE);

                pgtre_scan_and_sort(&wstate, true);
                tuplesort_performsort(wstate.sortstate);

                /* Report the leader-worker's tuple counts. */
                SpinLockAcquire(&bstate.pgtshared->mutex);
                bstate.pgtshared->nparticipantsdone++;
                bstate.pgtshared->reltuples += wstate.heap_tuples;
                bstate.pgtshared->indtuples += (double) wstate.n_emitted;
                SpinLockRelease(&bstate.pgtshared->mutex);
                ConditionVariableSignal(&bstate.pgtshared->workersdonecv);

                tuplesort_end(wstate.sortstate);
                MemoryContextDelete(wstate.tmpctx);
            }

            /* Wait for every participant. */
            {
                double indtuples = 0.0;
                bstate.heap_tuples = pgtre_parallel_heapscan(leader,
                                                             &indtuples);
            }

            ereport(NOTICE,
                    (errmsg("pg_tre: parallel build collected trigrams from "
                            "%.0f heap tuples across %d participants",
                            bstate.heap_tuples,
                            leader->nparticipanttuplesorts)));

            /* Merge every worker's partial run in the leader-role sort. */
            tuplesort_performsort(bstate.sortstate);
        }
        else
        {
            /* Serial build. */
            bstate.sortstate = tuplesort_begin_datum(BYTEAOID, tc->lt_opr,
                                                     InvalidOid, false,
                                                     maintenance_work_mem,
                                                     NULL, TUPLESORT_NONE);

            /* Step 3: scan heap and collect trigrams. */
            pgtre_scan_and_sort(&bstate, true);

            ereport(NOTICE,
                    (errmsg("pg_tre: collected %lld trigram entries from %.0f heap tuples",
                            (long long) bstate.n_emitted, bstate.heap_tuples)));

            /* Step 4: finish the sort. */
            tuplesort_performsort(bstate.sortstate);
        }
    }

    /* Step 5: process sorted entries and build posting trees. */
    oldcxt = MemoryContextSwitchTo(bstate.tmpctx);

    accums_alloced = 1024;
    accums = (PostingAccum *) palloc(accums_alloced * sizeof(PostingAccum));
    n_accums = 0;
    n_skipped_trigrams = 0;

    current_hash = 0;
    current_builder = NULL;

    /*
     * v2.0 posting-page coalescing (off by default).  When enabled, the
     * medium-bucket postings are packed onto shared coalesced pages
     * instead of one dedicated leaf each.  The writer batches across
     * trigrams and is flushed before the upper-tree bulkload.
     */
    if (pg_tre_coalesce_enable)
        coalesce_writer = pg_tre_coalesced_writer_begin(index);

    /* Read the sorted (trigram_hash, tid) stream and build posting trees.
     * Positions in the sort key are ignored (per-tuple payload removed). */
    {
        uint64 current_tid_packed = UINT64_MAX;   /* sentinel: no TID yet */
        TrigramTidEntry cur;
        Datum   sort_datum;
        bool    sort_isnull;

        while (tuplesort_getdatum(bstate.sortstate, true, false,
                                  &sort_datum, &sort_isnull, NULL))
        {
            bytea  *kb = DatumGetByteaPP(sort_datum);
            uint64  dec_hash;
            uint64  dec_tid;
            uint64  dec_key;
            TrigramTidEntry *entry = &cur;
            uint64  tid_packed;
            bool    new_trigram;
            bool    new_tid;

            /*
             * Allow CREATE INDEX [CONCURRENTLY] to be cancelled.  On
             * large heaps this readout runs for minutes; without an
             * interrupt check pg_cancel_backend / pg_terminate_backend
             * are silently ignored.
             */
            CHECK_FOR_INTERRUPTS();

            Assert(!sort_isnull);
            Assert(VARSIZE_ANY_EXHDR(kb) == PG_TRE_SORTKEY_LEN);
            decode_entry((const uint8 *) VARDATA_ANY(kb),
                         &dec_hash, &dec_tid, &dec_key);
            cur.trigram_hash = dec_hash;
            pg_tre_unpack_tid(dec_tid, &cur.tid);
            cur.position = 0;

            tid_packed = dec_tid;
            new_trigram = (current_builder == NULL ||
                           entry->trigram_hash != current_hash);
            new_tid = (new_trigram || tid_packed != current_tid_packed);

            /*
             * Collect the order-preserving key of each distinct trigram for
             * the SuRF filter.  A trigram_hash uniquely identifies a
             * trigram, so "new_trigram" (hash changed) marks a new distinct
             * key.  Keys arrive grouped by hash (not sorted by key), so we
             * append here and sort+dedup after the loop.
             */
            if (new_trigram)
            {
                if (surf_n >= surf_cap)
                {
                    surf_cap = surf_cap ? surf_cap * 2 : 1024;
                    surf_keys = surf_keys
                        ? (uint64 *) repalloc(surf_keys, surf_cap * sizeof(uint64))
                        : (uint64 *) palloc(surf_cap * sizeof(uint64));
                }
                surf_keys[surf_n++] = dec_key;
            }

            if (new_trigram)
            {
                /* Finish previous trigram's posting tree. */
                if (current_builder != NULL)
                {
                    /* Add the last accumulated TID. */
                    if (current_tid_packed != UINT64_MAX)
                    {
                        ItemPointerData tid;

                        pg_tre_unpack_tid(current_tid_packed, &tid);
                        pg_tre_posting_build_add(current_builder, &tid,
                                                NULL, 0, NULL);
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
                            bool        coalesced;
                            BlockNumber cblk;
                            uint16      cslot;

                            root = pg_tre_posting_build_finish_ex(
                                       current_builder,
                                       &inline_data,
                                       &inline_bytes,
                                       coalesce_writer,
                                       &coalesced, &cblk, &cslot);
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
                            if (coalesced)
                            {
                                accums[n_accums].root = cblk;
                                accums[n_accums].inline_data = NULL;
                                accums[n_accums].inline_bytes =
                                    PG_TRE_COALESCED_FLAG | cslot;
                            }
                            else
                            {
                                accums[n_accums].root = root;
                                accums[n_accums].inline_data = inline_data;
                                accums[n_accums].inline_bytes = inline_bytes;
                            }
                            n_accums++;
                        }
                    }
                }

                /* Start a new posting tree. */
                current_hash = entry->trigram_hash;
                current_builder = pg_tre_posting_build_begin(
                                      index, current_hash,
                                      false /* no payload (3.0.0) */);
                current_tid_packed = UINT64_MAX;  /* reset for new trigram */
            }

            if (new_tid && !new_trigram)
            {
                /* Flush the previous TID into the posting set. */
                if (current_tid_packed != UINT64_MAX)
                {
                    ItemPointerData tid;

                    pg_tre_unpack_tid(current_tid_packed, &tid);
                    pg_tre_posting_build_add(current_builder, &tid,
                                            NULL, 0, NULL);
                }
            }

            /* Track the current TID; positions are no longer captured
             * (per-tuple payload was removed in 3.0.0). */
            current_tid_packed = tid_packed;
        }

        /* Finish the last posting tree. */
        if (current_builder != NULL)
        {
            /* Add the last accumulated TID. */
            if (current_tid_packed != UINT64_MAX)
            {
                ItemPointerData tid;

                pg_tre_unpack_tid(current_tid_packed, &tid);
                pg_tre_posting_build_add(current_builder, &tid,
                                        NULL, 0, NULL);
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
                    bool        coalesced;
                    BlockNumber cblk;
                    uint16      cslot;

                    root = pg_tre_posting_build_finish_ex(current_builder,
                                                          &inline_data,
                                                          &inline_bytes,
                                                          coalesce_writer,
                                                          &coalesced,
                                                          &cblk, &cslot);
                    pg_tre_posting_build_free(current_builder);

                    if (n_accums >= accums_alloced)
                    {
                        accums_alloced *= 2;
                        accums = (PostingAccum *)
                            repalloc(accums,
                                     accums_alloced * sizeof(PostingAccum));
                    }
                    accums[n_accums].trigram_hash = current_hash;
                    if (coalesced)
                    {
                        accums[n_accums].root = cblk;
                        accums[n_accums].inline_data = NULL;
                        accums[n_accums].inline_bytes =
                            PG_TRE_COALESCED_FLAG | cslot;
                    }
                    else
                    {
                        accums[n_accums].root = root;
                        accums[n_accums].inline_data = inline_data;
                        accums[n_accums].inline_bytes = inline_bytes;
                    }
                    n_accums++;
                }
            }
        }
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

    /* Flush the coalesced writer's last partial page so all coalesced
     * pages exist before the upper-tree bulkload references them. */
    if (coalesce_writer != NULL)
    {
        pg_tre_coalesced_writer_finish(coalesce_writer);
        coalesce_writer = NULL;
    }

    /* Step 6: bulk-load upper tree from the posting list. */
    iter_state.accums = accums;
    iter_state.n_accums = n_accums;
    iter_state.current = 0;

    root_upper = pg_tre_upper_bulkload(index, upper_iter, &iter_state);

    /*
     * Step 6.5: build the v10 SuRF range filter over the order-preserving
     * trigram keys.  (The BRIN-style range-bloom tier that used to live
     * here was removed in 3.2.0 -- it was never consulted at scan time, so
     * root_range is always InvalidBlockNumber now.)  Keys were collected
     * from the merged stream grouped by hash; sort + dedup, build the SuRF,
     * and persist it to a SURF page chain.  An empty index writes no SuRF
     * (root_surf stays InvalidBlockNumber -> scans skip the prefilter).
     */
    {
        BlockNumber root_surf = InvalidBlockNumber;
        uint32      surf_distinct = 0;

        if (surf_n > 0)
        {
            PgTreSurf  *surf;
            uint32      i,
                        m;
            Size        img_len;
            uint8      *img;

            qsort(surf_keys, surf_n, sizeof(uint64), cmp_uint64);
            m = 0;
            for (i = 0; i < surf_n; i++)
                if (m == 0 || surf_keys[i] != surf_keys[m - 1])
                    surf_keys[m++] = surf_keys[i];
            surf_distinct = m;

            surf = pg_tre_surf_build(surf_keys, m);
            img_len = pg_tre_surf_serialized_size(surf);
            img = (uint8 *) palloc(img_len);
            pg_tre_surf_serialize(surf, img);
            root_surf = pg_tre_surf_write_image(index, img, img_len);
            pfree(img);
            pg_tre_surf_free(surf);

            ereport(DEBUG1,
                    (errmsg("pg_tre: built SuRF filter over %u distinct "
                            "trigram keys (%zu bytes)",
                            surf_distinct, img_len)));
        }

        /* Step 7: update meta page with roots and stats. */
        pg_tre_meta_set_roots(index, root_upper, InvalidBlockNumber,
                              (uint64) n_accums, (uint64) bstate.heap_tuples);
        pg_tre_meta_set_surf(index, root_surf, surf_distinct);
    }

    MemoryContextSwitchTo(oldcxt);
    MemoryContextDelete(bstate.tmpctx);

    tuplesort_end(bstate.sortstate);

    /* Tear down parallel workers now that the shared sort is fully drained. */
    if (build_leader != NULL)
        pgtre_end_parallel(build_leader);

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

/*
 * tre_estimate_index_build(rel regclass, attno int) -> record
 *
 * Up-front sizing precheck for a TRE index build (customer ask:
 * "tell me before I start whether it will fit").  Samples up to
 * TRE_ESTIMATE_SAMPLE_ROWS rows of the target text column, counts
 * distinct trigrams per row, and extrapolates to the whole table.
 *
 * Returns:
 *   sample_rows      rows actually sampled
 *   est_rows         relation live-tuple estimate
 *   est_trigrams     extrapolated distinct (trigram,tid) emissions
 *   est_temp_mb      estimated build temp-disk (emissions * ~64 B)
 *   est_index_mb     rough final index size estimate
 *
 * The temp figure uses the same per-tuple cost the build's
 * build_max_entries_mb ceiling uses, so an operator can size
 * build_max_entries_mb / temp tablespace before committing.
 */
#define TRE_ESTIMATE_SAMPLE_ROWS 2000

PG_FUNCTION_INFO_V1(tre_estimate_index_build);
Datum
tre_estimate_index_build(PG_FUNCTION_ARGS)
{
    Oid             relid = PG_GETARG_OID(0);
    int             attno = PG_ARGISNULL(1) ? 1 : PG_GETARG_INT32(1);
    Relation        rel;
    TableScanDesc   scan;
    TupleTableSlot *slot;
    int64           sampled = 0;
    int64           sample_trigrams = 0;   /* distinct (trigram) over sample */
    double          rel_tuples;
    int64           est_trigrams;
    int64           est_temp_mb;
    int64           est_index_mb;
    TupleDesc       resdesc;
    Datum           vals[5];
    bool            nulls[5] = {false, false, false, false, false};
    HeapTuple       restup;

    if (get_call_result_type(fcinfo, NULL, &resdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "tre_estimate_index_build must return a record type");
    resdesc = BlessTupleDesc(resdesc);

    rel = relation_open(relid, AccessShareLock);
    rel_tuples = rel->rd_rel->reltuples > 0
                 ? (double) rel->rd_rel->reltuples : 0.0;

    slot = table_slot_create(rel, NULL);
    scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL);

    while (sampled < TRE_ESTIMATE_SAMPLE_ROWS &&
           table_scan_getnextslot(scan, ForwardScanDirection, slot))
    {
        bool    isnull;
        Datum   v;

        CHECK_FOR_INTERRUPTS();
        v = slot_getattr(slot, attno, &isnull);
        sampled++;
        if (isnull)
            continue;
        {
            text         *txt = DatumGetTextPP(v);
            const char   *str = VARDATA_ANY(txt);
            int           len = VARSIZE_ANY_EXHDR(txt);
            PgTreCpStream stream;
            int32         ring[3];
            int           ring_n = 0;
            int32         cp;
            uint64       *seen;
            uint32        cap = 16, mask, n = 0;
            bool          seen_zero = false;
            uint32        want = (uint32) (len > 0 ? len : 1) + 8;

            while (cap < want)
                cap <<= 1;
            if (cap > (1u << 22))
                cap = (1u << 22);
            mask = cap - 1;
            seen = (uint64 *) palloc0(sizeof(uint64) * cap);

            pg_tre_cpstream_init(&stream, str, len);
            for (;;)
            {
                cp = pg_tre_cpstream_next(&stream);
                if (cp < 0)
                    break;
                if (ring_n >= 3)
                {
                    ring[0] = ring[1]; ring[1] = ring[2]; ring[2] = cp;
                }
                else
                {
                    ring[ring_n++] = cp;
                }
                if (ring_n == 3)
                {
                    uint64 h = pg_tre_hash_trigram_cp(ring);
                    if (h == 0)
                    {
                        if (!seen_zero) { seen_zero = true; sample_trigrams++; }
                    }
                    else if (n < cap)
                    {
                        uint32 slot2 = (uint32) (h * 0x9E3779B97F4A7C15ULL >> 40) & mask;
                        bool found = false;
                        uint32 probes = 0;
                        while (seen[slot2] != 0)
                        {
                            if (seen[slot2] == h) { found = true; break; }
                            slot2 = (slot2 + 1) & mask;
                            if (++probes >= cap) break;
                        }
                        if (!found && seen[slot2] == 0)
                        {
                            seen[slot2] = h; n++; sample_trigrams++;
                        }
                    }
                }
            }
            pfree(seen);
        }
    }

    table_endscan(scan);
    ExecDropSingleTupleTableSlot(slot);
    relation_close(rel, AccessShareLock);

    /* Extrapolate.  If reltuples is unknown (0), report per-sample only. */
    if (rel_tuples <= 0)
        rel_tuples = (double) sampled;
    if (sampled > 0)
        est_trigrams = (int64) ((double) sample_trigrams / (double) sampled
                                * rel_tuples);
    else
        est_trigrams = 0;
    est_temp_mb  = (int64) ((double) est_trigrams * PG_TRE_SORT_TUPLE_TEMP_BYTES
                            / (1024.0 * 1024.0));
    /* Final index: distinct trigrams collapse into posting trees; a
     * conservative rough estimate is ~16 bytes per (trigram,tid)
     * after sparsemap compression of the TID lists. */
    est_index_mb = (int64) ((double) est_trigrams * 16.0
                            / (1024.0 * 1024.0));

    vals[0] = Int64GetDatum(sampled);
    vals[1] = Int64GetDatum((int64) rel_tuples);
    vals[2] = Int64GetDatum(est_trigrams);
    vals[3] = Int64GetDatum(est_temp_mb);
    vals[4] = Int64GetDatum(est_index_mb);
    restup = heap_form_tuple(resdesc, vals, nulls);
    PG_RETURN_DATUM(HeapTupleGetDatum(restup));
}
