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
#include "utils/tuplesort.h"
#include "utils/typcache.h"

#include "funcapi.h"
#include "access/relation.h"

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
 *   bytes 16..19 : position              (uint32, big-endian)
 *
 * pg_tre_pack_tid() is (block << 16) | offset, so the numeric order
 * of the packed value equals ItemPointerCompare order, and big-endian
 * bytes preserve that under memcmp.
 */
#define PG_TRE_SORTKEY_LEN 20

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

static inline void
encode_entry(uint8 *buf, uint64 trigram_hash, uint64 packed_tid,
             uint32 position)
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
    buf[16] = (uint8) (position >> 24);
    buf[17] = (uint8) (position >> 16);
    buf[18] = (uint8) (position >> 8);
    buf[19] = (uint8) (position);
}

static inline void
decode_entry(const uint8 *buf, uint64 *trigram_hash, uint64 *packed_tid,
             uint32 *position)
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
    *position =
          ((uint32) buf[16] << 24) | ((uint32) buf[17] << 16)
        | ((uint32) buf[18] << 8)  | ((uint32) buf[19]);
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
     * indexed text).  We now emit each distinct (trigram, tid) once,
     * keeping the FIRST position.  Dropping the later positions is
     * correctness-safe: per-occurrence positions only feed the
     * optional tier-3.1 positional filter, which is a lossy
     * refinement over candidates the executor rechecks anyway --
     * fewer positions means at most less pre-filtering, never a
     * missed match.
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
                         (uint32) ring_pos[0]);
            SET_VARSIZE(&wrap, VARHDRSZ + PG_TRE_SORTKEY_LEN);
            memcpy(wrap.data, key, PG_TRE_SORTKEY_LEN);
            tuplesort_putdatum(bstate->sortstate,
                               PointerGetDatum(&wrap), false);

            /* Phase 5: add trigram to the per-tuple bloom. */
            if (bloom != NULL)
                pg_tre_bloom_add_trigram(bloom, trigram_hash);

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

    /* Phase 2 real build starts here. */

    /* Step 1: initialize empty meta page. */
    pg_tre_build_empty(index);

    /* Step 2: set up the disk-spillable sort and bloom tracking. */
    bstate.heap = heap;
    bstate.index = index;
    bstate.indexInfo = indexInfo;
    bstate.n_emitted = 0;

    /*
     * Sort (trigram_hash, tid, position) tuples encoded as fixed
     * 20-byte big-endian bytea Datums (see encode_entry).  tuplesort
     * bounds peak memory by maintenance_work_mem and spills the rest
     * to temp files, so the build no longer grows resident memory
     * with the corpus size.  bytea's memcmp ordering reproduces the
     * historical (hash, packed_tid, position) order exactly.
     */
    {
        TypeCacheEntry *tc = lookup_type_cache(BYTEAOID, TYPECACHE_LT_OPR);

        if (!OidIsValid(tc->lt_opr))
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("pg_tre: no btree \"<\" operator for bytea")));

        bstate.sortstate = tuplesort_begin_datum(BYTEAOID,
                                                 tc->lt_opr,
                                                 InvalidOid,   /* no collation */
                                                 false,        /* nulls last */
                                                 maintenance_work_mem,
                                                 NULL,         /* not parallel */
                                                 TUPLESORT_NONE);
    }

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
            (errmsg("pg_tre: collected %lld trigram entries from %.0f heap tuples",
                    (long long) bstate.n_emitted, bstate.heap_tuples)));

    /* Step 4: finish the sort.  tuplesort_performsort() spills to disk
     * as needed and checks for interrupts internally, so the sort is
     * both memory-bounded (maintenance_work_mem) and cancellable. */
    tuplesort_performsort(bstate.sortstate);

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
        TrigramTidEntry cur;
        Datum   sort_datum;
        bool    sort_isnull;

        while (tuplesort_getdatum(bstate.sortstate, true, false,
                                  &sort_datum, &sort_isnull, NULL))
        {
            bytea  *kb = DatumGetByteaPP(sort_datum);
            uint64  dec_hash;
            uint64  dec_tid;
            uint32  dec_pos;
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
                         &dec_hash, &dec_tid, &dec_pos);
            cur.trigram_hash = dec_hash;
            pg_tre_unpack_tid(dec_tid, &cur.tid);
            cur.position = dec_pos;

            tid_packed = dec_tid;
            new_trigram = (current_builder == NULL ||
                           entry->trigram_hash != current_hash);
            new_tid = (new_trigram || tid_packed != current_tid_packed);

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

    tuplesort_end(bstate.sortstate);

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
