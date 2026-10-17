/*
 * src/am/ambuild.c - index build.
 *
 *   1. Initialize the meta page.
 *   2. Scan the heap (in parallel when workers are available), extract the
 *      codepoint trigrams of each row, and feed (trigram_hash, tid, key)
 *      to a disk-spillable tuplesort.
 *   3. Read the sorted stream back and write one posting tree per distinct
 *      trigram.  A parallel build routes each trigram to one of several
 *      hash-range sorts and builds the ranges on every participant at once
 *      (doc/design/parallel-posting-build.md).
 *   4. Bulk-load the upper tree from the (hash, root|inline) list in hash
 *      order, build the SuRF prefix filter, and update the meta page.
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
#include "storage/buffile.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/sharedfileset.h"
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

    /*
     * Parallel participant: one worker-role sort per trigram-hash range
     * (see doc/design/parallel-posting-build.md); a trigram goes to sort
     * hash / range_width.  nranges == 0 means "use sortstate".
     */
    Tuplesortstate **rsorts;
    int         nranges;
    uint64      range_width;

    double      heap_tuples;
    MemoryContext tmpctx;
    MemoryContext rowctx;        /* per-heap-tuple scratch, reset per row */

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

    /* Trigram-hash ranges: one shared sort each, sortstride bytes apart. */
    int         nranges;
    Size        sortstride;

    /* Per-range results (posting list + SuRF keys), one BufFile each. */
    SharedFileSet resultfs;
    ConditionVariable rangesdonecv;

    /* Mutable state, protected by mutex. */
    int         nparticipantsdone;
    int         nparticipants;   /* leader + launched workers; 0 = not yet known */
    int         nextrange;       /* next range to claim */
    int         rangesdone;
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
 * Hash ranges per planned participant in a parallel build.  Each range is a
 * sort per participant, so more ranges means smaller sort memory each.
 * ponytail: fixed ratio; size ranges from a hash sample if skew at scale
 * proves this too coarse.
 */
#define PG_TRE_RANGES_PER_PARTICIPANT 4

/* The shared sort of hash range r; ranges are laid out stride bytes apart. */
#define RANGE_SORT(base, stride, r) \
    ((Sharedsort *) ((char *) (base) + (Size) (r) * (stride)))

/*
 * Leader-private state for a parallel build: the ParallelContext plus
 * pointers into the DSM segment.  Mirrors nbtree's BTLeader.
 */
typedef struct PgTreLeader
{
    ParallelContext *pcxt;
    int              nparticipanttuplesorts;
    PgTreShared     *pgtshared;
    Sharedsort      *sharedsort;     /* range 0; others sortstride apart */
    int              nranges;
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
 * The key's TID is packed LEGACY, (block << 16) | offset, so the numeric order
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
                                 "host with enough RAM/temp space, index a "
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
            /* The sort key's TID is transient: LEGACY packing is lossless
             * for any offset and orders TIDs exactly like the index's own
             * packing, so the posting builder can repack it. */
            encode_entry(key, trigram_hash,
                         pg_tre_pack_tid(PG_TRE_TID_LEGACY, tid),
                         pg_tre_trigram_key_cp(ring));
            SET_VARSIZE(&wrap, VARHDRSZ + PG_TRE_SORTKEY_LEN);
            memcpy(wrap.data, key, PG_TRE_SORTKEY_LEN);
            tuplesort_putdatum(bstate->nranges > 0
                               ? bstate->rsorts[trigram_hash / bstate->range_width]
                               : bstate->sortstate,
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
    MemoryContext oldcxt;

    /*
     * Heap scan can run for minutes on large relations; without an
     * interrupt check the backend ignores pg_cancel_backend /
     * pg_terminate_backend until the entire scan completes.  Same
     * defense as the post-sort loop below.
     */
    CHECK_FOR_INTERRUPTS();

    /*
     * Per-tuple memory, reset after every row (as nbtree and GIN do).
     * extract_trigrams detoasts the value, and a compressed or out-of-line
     * body is decompressed into CurrentMemoryContext -- before this, the
     * statement's PortalContext, so every such row leaked its full text
     * for the whole scan.  A 30 GB corpus with half its rows compressed
     * grew the build backend by ~5 MB/s to tens of GB (issue #1).
     * tuplesort_putdatum copies the key into the sort's own memory, so
     * nothing allocated here outlives the row.
     */
    oldcxt = MemoryContextSwitchTo(bstate->rowctx);

    /*
     * Phase 2: only index the first column (assume it's text).
     * Later phases handle multi-column and operator classes properly.
     */
    if (bstate->indexInfo->ii_NumIndexAttrs >= 1)
    {
        extract_trigrams(bstate, values[0], isnull[0], tid);
    }

    MemoryContextSwitchTo(oldcxt);
    MemoryContextReset(bstate->rowctx);

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

/*
 * Output of the posting phase for one stream (the whole sort in a serial
 * build, one hash range in a parallel one): the (hash, root|inline) list
 * the upper tree is built from, in ascending hash order, and the SuRF keys
 * of every distinct trigram (skipped ones included).
 */
typedef struct PostingOut
{
    PostingAccum *accums;
    int         n_accums;
    int         accums_alloced;
    uint64     *surf_keys;
    uint32      surf_n;
    uint32      surf_cap;
    int         n_skipped;      /* below pg_tre.min_trigram_freq */
} PostingOut;

static void
posting_out_init(PostingOut *out)
{
    memset(out, 0, sizeof(*out));
    out->accums_alloced = 1024;
    out->accums = (PostingAccum *) palloc(out->accums_alloced * sizeof(PostingAccum));
}

static void
posting_out_add(PostingOut *out, uint64 hash, BlockNumber root,
                const uint8 *inline_data, Size inline_bytes)
{
    if (out->n_accums >= out->accums_alloced)
    {
        out->accums_alloced *= 2;
        out->accums = (PostingAccum *)
            repalloc_huge(out->accums,
                          (Size) out->accums_alloced * sizeof(PostingAccum));
    }
    out->accums[out->n_accums].trigram_hash = hash;
    out->accums[out->n_accums].root = root;
    out->accums[out->n_accums].inline_data = inline_data;
    out->accums[out->n_accums].inline_bytes = inline_bytes;
    out->n_accums++;
}

static void
posting_out_add_key(PostingOut *out, uint64 key)
{
    if (out->surf_n >= out->surf_cap)
    {
        out->surf_cap = out->surf_cap ? out->surf_cap * 2 : 1024;
        out->surf_keys = out->surf_keys
            ? (uint64 *) repalloc_huge(out->surf_keys,
                                       (Size) out->surf_cap * sizeof(uint64))
            : (uint64 *) palloc(out->surf_cap * sizeof(uint64));
    }
    out->surf_keys[out->surf_n++] = key;
}

/* Write one trigram's posting (or skip it) and record the result. */
static void
finish_posting(PostingOut *out, PgTrePostingBuilder *b, uint64 hash,
               PgTreCoalescedWriter *cw)
{
    const uint8 *inline_data;
    Size        inline_bytes;
    BlockNumber root;
    bool        coalesced;
    BlockNumber cblk;
    uint16      cslot;

    /*
     * Cardinality-aware build (1.2.1-4.4.x).  Unreachable since 4.5.0:
     * pg_tre.min_trigram_freq's check hook refuses values above 1, because
     * a dropped posting made scans treat the trigram as absent from every
     * row (the recheck never saw those rows).
     */
    if (pg_tre_min_trigram_freq > 1 &&
        pg_tre_posting_build_n_tids(b) < pg_tre_min_trigram_freq)
    {
        pg_tre_posting_build_free(b);
        out->n_skipped++;
        return;
    }

    root = pg_tre_posting_build_finish_ex(b, &inline_data, &inline_bytes,
                                          cw, &coalesced, &cblk, &cslot);
    pg_tre_posting_build_free(b);
    if (coalesced)
        posting_out_add(out, hash, cblk, NULL, PG_TRE_COALESCED_FLAG | cslot);
    else
        posting_out_add(out, hash, root, inline_data, inline_bytes);
}

/*
 * Step 5: read a sorted (trigram_hash, tid, key) stream and write one
 * posting tree per distinct trigram.  Allocates in the current context.
 */
static void
build_postings(Tuplesortstate *sortstate, Relation index, PostingOut *out)
{
    PgTrePostingBuilder *builder = NULL;
    PgTreCoalescedWriter *cw = NULL;
    uint64      cur_hash = 0;
    uint64      last_tid = UINT64_MAX;      /* none yet */
    Datum       sort_datum;
    bool        sort_isnull;

    /*
     * v2.0 posting-page coalescing (off by default): medium postings pack
     * onto shared coalesced pages instead of one dedicated leaf each.
     */
    if (pg_tre_coalesce_enable)
        cw = pg_tre_coalesced_writer_begin(index);

    while (tuplesort_getdatum(sortstate, true, false,
                              &sort_datum, &sort_isnull, NULL))
    {
        bytea      *kb = DatumGetByteaPP(sort_datum);
        uint64      hash;
        uint64      tid;
        uint64      key;

        /*
         * Allow CREATE INDEX [CONCURRENTLY] to be cancelled: on large heaps
         * this readout runs for minutes.
         */
        CHECK_FOR_INTERRUPTS();

        Assert(!sort_isnull);
        Assert(VARSIZE_ANY_EXHDR(kb) == PG_TRE_SORTKEY_LEN);
        decode_entry((const uint8 *) VARDATA_ANY(kb), &hash, &tid, &key);

        if (builder == NULL || hash != cur_hash)
        {
            if (builder != NULL)
                finish_posting(out, builder, cur_hash, cw);
            cur_hash = hash;
            /* A trigram_hash identifies a trigram: one SuRF key each. */
            posting_out_add_key(out, key);
            builder = pg_tre_posting_build_begin(index, hash,
                                                 false /* no payload (3.0.0) */);
            last_tid = UINT64_MAX;
        }
        if (tid != last_tid)
        {
            ItemPointerData itid;

            pg_tre_unpack_tid(PG_TRE_TID_LEGACY, tid, &itid);
            pg_tre_posting_build_add(builder, &itid, NULL, 0, NULL);
            last_tid = tid;
        }
    }
    if (builder != NULL)
        finish_posting(out, builder, cur_hash, cw);

    /* Flush the coalesced writer's last partial page so all coalesced
     * pages exist before the upper-tree bulkload references them. */
    if (cw != NULL)
        pg_tre_coalesced_writer_finish(cw);
}

/* ---------------------------------------------------------------------
 * Parallel build support.
 *
 * All participants (leader + workers) scan a slice of the heap and feed
 * one worker-role sort per trigram-hash range.  Once every participant has
 * sorted, each claims ranges one at a time and builds their posting trees;
 * the leader then reads the ranges' results back in hash order for the
 * upper tree and SuRF.  The launch/teardown follows nbtree's
 * _bt_begin_parallel / _bt_parallel_scan_and_sort.
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
    int              nranges;
    Size             sortstride;
    int              r;

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

    /*
     * Tuplesort coordination: one shared sort per trigram-hash range, so
     * the posting phase can build ranges on every participant at once.
     * Participants claim ranges as they go, so a participant that never
     * launches just leaves its share to the others.  Trigram hashes are
     * far from uniform in their high bits, and the TID volume per trigram
     * is Zipf-skewed, so equal hash ranges are unequal work: with one
     * range per participant the largest kept the posting phase waiting
     * on one core.  PG_TRE_RANGES_PER_PARTICIPANT finer ranges, claimed
     * dynamically, even that out.
     */
    nranges = scantuplesortstates * PG_TRE_RANGES_PER_PARTICIPANT;
    sortstride = BUFFERALIGN(tuplesort_estimate_shared(scantuplesortstates));
    estsort = mul_size(sortstride, nranges);
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
    pgtshared->nparticipants = 0;
    pgtshared->nranges = nranges;
    pgtshared->sortstride = sortstride;
    pgtshared->nextrange = 0;
    pgtshared->rangesdone = 0;
    SharedFileSetInit(&pgtshared->resultfs, pcxt->seg);
    ConditionVariableInit(&pgtshared->rangesdonecv);
    pgtshared->reltuples = 0.0;
    pgtshared->indtuples = 0.0;

    /* Parallel heap scan descriptor lives immediately after the struct. */
    pscan = (ParallelTableScanDesc) ((char *) pgtshared + BUFFERALIGN(sizeof(PgTreShared)));
    table_parallelscan_initialize(bstate->heap, pscan, snapshot);

    shm_toc_insert(pcxt->toc, PG_TRE_BUILD_KEY_SHARED, pgtshared);

    /* Set up the shared tuplesort coordination object. */
    sharedsort = (Sharedsort *) shm_toc_allocate(pcxt->toc, estsort);
    for (r = 0; r < nranges; r++)
        tuplesort_initialize_shared(RANGE_SORT(sharedsort, sortstride, r),
                                    scantuplesortstates, pcxt->seg);
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
    leader->nranges = nranges;
    leader->snapshot = snapshot;
    leader->walusage = walusage;
    leader->bufferusage = bufferusage;

    /* If no workers actually started, tear down and fall back to serial. */
    if (pcxt->nworkers_launched == 0)
    {
        pgtre_end_parallel(leader);
        return NULL;
    }

    /* Participants may only claim ranges once this is known. */
    SpinLockAcquire(&pgtshared->mutex);
    pgtshared->nparticipants = leader->nparticipanttuplesorts;
    SpinLockRelease(&pgtshared->mutex);

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
    bstate->rowctx = AllocSetContextCreate(CurrentMemoryContext,
                                           "pg_tre build per-row context",
                                           ALLOCSET_DEFAULT_SIZES);
    if (bstate->pgtshared != NULL)
    {
        /* Parallel: scan our slice of the shared parallel scan. */
        ParallelTableScanDesc pscan =
            (ParallelTableScanDesc) ((char *) bstate->pgtshared +
                                     BUFFERALIGN(sizeof(PgTreShared)));
        TableScanDesc scan = table_beginscan_parallel(bstate->heap, pscan
#if PG_VERSION_NUM >= 190000
                                                      , SO_NONE
#endif
                                                      );

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
    MemoryContextDelete(bstate->rowctx);
    bstate->rowctx = NULL;
}

/*
 * Range-partitioned posting phase (doc/design/parallel-posting-build.md).
 *
 * Each participant scans its heap slice into one worker-role sort per
 * trigram-hash range, signals, waits until every participant has, then
 * claims ranges one at a time: a leader-role merge of that range's sort
 * builds its posting trees, and the (hash, root|inline) list plus the SuRF
 * keys go to a BufFile the leader reads back in range order.
 */

/* Open this participant's worker-role sorts, one per range. */
static void
pgtre_begin_range_sorts(BuildState *st, PgTreShared *shared,
                        Sharedsort *base, Oid lt_opr, int sortmem)
{
    int         r;

    st->nranges = shared->nranges;
    st->range_width = PG_UINT64_MAX / (uint64) shared->nranges + 1;
    st->rsorts = (Tuplesortstate **) palloc(sizeof(Tuplesortstate *) * st->nranges);
    for (r = 0; r < st->nranges; r++)
    {
        SortCoordinate coord = palloc0(sizeof(SortCoordinateData));

        coord->isWorker = true;
        coord->nParticipants = -1;
        coord->sharedsort = RANGE_SORT(base, shared->sortstride, r);
        st->rsorts[r] = tuplesort_begin_datum(BYTEAOID, lt_opr, InvalidOid,
                                              false,
                                              Max(sortmem / st->nranges, 64),
                                              coord, TUPLESORT_NONE);
    }
}

/* Sort every range run and report the scan to the others. */
static void
pgtre_finish_range_sorts(BuildState *st, PgTreShared *shared)
{
    int         r;

    for (r = 0; r < st->nranges; r++)
    {
        tuplesort_performsort(st->rsorts[r]);
        tuplesort_end(st->rsorts[r]);
    }

    SpinLockAcquire(&shared->mutex);
    shared->nparticipantsdone++;
    shared->reltuples += st->heap_tuples;
    shared->indtuples += (double) st->n_emitted;
    SpinLockRelease(&shared->mutex);
    ConditionVariableBroadcast(&shared->workersdonecv);
}

static void
range_file_name(char *name, int r)
{
    snprintf(name, MAXPGPATH, "pg_tre-range-%d", r);
}

/*
 * Claim and build ranges until none is left.  Waits first for every
 * participant's scan: a range's leader-role merge needs all their runs.
 */
static void
pgtre_build_ranges(PgTreShared *shared, Sharedsort *base, Relation index,
                   Oid lt_opr)
{
    for (;;)
    {
        int         nparticipants;
        int         done;

        SpinLockAcquire(&shared->mutex);
        nparticipants = shared->nparticipants;
        done = shared->nparticipantsdone;
        SpinLockRelease(&shared->mutex);
        if (nparticipants > 0 && done == nparticipants)
            break;
        ConditionVariableSleep(&shared->workersdonecv,
                               WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
    }
    ConditionVariableCancelSleep();

    for (;;)
    {
        int         r;
        int         nparticipants;
        SortCoordinate coord;
        Tuplesortstate *sort;
        MemoryContext cxt;
        MemoryContext old;
        PostingOut  out;
        BufFile    *f;
        char        name[MAXPGPATH];
        int         i;

        SpinLockAcquire(&shared->mutex);
        r = shared->nextrange++;
        nparticipants = shared->nparticipants;
        SpinLockRelease(&shared->mutex);
        if (r >= shared->nranges)
            break;

        cxt = AllocSetContextCreate(CurrentMemoryContext,
                                    "pg_tre range build",
                                    ALLOCSET_DEFAULT_SIZES);
        old = MemoryContextSwitchTo(cxt);

        coord = palloc0(sizeof(SortCoordinateData));
        coord->isWorker = false;
        coord->nParticipants = nparticipants;
        coord->sharedsort = RANGE_SORT(base, shared->sortstride, r);
        sort = tuplesort_begin_datum(BYTEAOID, lt_opr, InvalidOid, false,
                                     maintenance_work_mem / nparticipants,
                                     coord, TUPLESORT_NONE);
        tuplesort_performsort(sort);

        posting_out_init(&out);
        build_postings(sort, index, &out);
        tuplesort_end(sort);
        ereport(DEBUG1,
                (errmsg("pg_tre: range %d/%d: %d posting trees",
                        r + 1, shared->nranges, out.n_accums)));

        /* counts, accums (+ inline bytes), keys */
        range_file_name(name, r);
        f = BufFileCreateFileSet(&shared->resultfs.fs, name);
        BufFileWrite(f, &out.n_accums, sizeof(out.n_accums));
        BufFileWrite(f, &out.n_skipped, sizeof(out.n_skipped));
        BufFileWrite(f, &out.surf_n, sizeof(out.surf_n));
        for (i = 0; i < out.n_accums; i++)
        {
            PostingAccum *a = &out.accums[i];
            uint32      ib = (uint32) a->inline_bytes;

            BufFileWrite(f, &a->trigram_hash, sizeof(a->trigram_hash));
            BufFileWrite(f, &a->root, sizeof(a->root));
            BufFileWrite(f, &ib, sizeof(ib));
            if (!(ib & PG_TRE_COALESCED_FLAG) && ib > 0 &&
                a->root == InvalidBlockNumber)
            {
                Assert(a->inline_data != NULL);
                BufFileWrite(f, a->inline_data, ib);     /* inline posting */
            }
        }
        if (out.surf_n > 0)
            BufFileWrite(f, out.surf_keys, sizeof(uint64) * out.surf_n);
        BufFileClose(f);

        MemoryContextSwitchTo(old);
        MemoryContextDelete(cxt);

        SpinLockAcquire(&shared->mutex);
        shared->rangesdone++;
        SpinLockRelease(&shared->mutex);
        ConditionVariableBroadcast(&shared->rangesdonecv);
    }
}

/*
 * Leader: build ranges alongside the workers, wait for all of them, and
 * read the results back in range (= hash) order into one PostingOut.
 */
static void
pgtre_parallel_postings(PgTreLeader *leader, Relation index, PostingOut *out)
{
    PgTreShared *shared = leader->pgtshared;
    TypeCacheEntry *tc = lookup_type_cache(BYTEAOID, TYPECACHE_LT_OPR);
    int         r;
    pgtre_build_ranges(shared, leader->sharedsort, index, tc->lt_opr);

    for (;;)
    {
        int         done;

        SpinLockAcquire(&shared->mutex);
        done = shared->rangesdone;
        SpinLockRelease(&shared->mutex);
        if (done == shared->nranges)
            break;
        ConditionVariableSleep(&shared->rangesdonecv,
                               WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
    }
    ConditionVariableCancelSleep();

    posting_out_init(out);
    for (r = 0; r < shared->nranges; r++)
    {
        char        name[MAXPGPATH];
        BufFile    *f;
        int         n_accums;
        int         n_skipped;
        uint32      surf_n;
        int         i;

        range_file_name(name, r);
        f = BufFileOpenFileSet(&shared->resultfs.fs, name, O_RDONLY, false);
        BufFileReadExact(f, &n_accums, sizeof(n_accums));
        BufFileReadExact(f, &n_skipped, sizeof(n_skipped));
        BufFileReadExact(f, &surf_n, sizeof(surf_n));
        out->n_skipped += n_skipped;
        for (i = 0; i < n_accums; i++)
        {
            uint64      hash;
            BlockNumber root;
            uint32      ib;
            uint8      *data = NULL;

            BufFileReadExact(f, &hash, sizeof(hash));
            BufFileReadExact(f, &root, sizeof(root));
            BufFileReadExact(f, &ib, sizeof(ib));
            if (!(ib & PG_TRE_COALESCED_FLAG) && ib > 0 &&
                root == InvalidBlockNumber)
            {
                /* +16: the zero tail the posting reader may touch */
                data = (uint8 *) palloc0(ib + 16);
                BufFileReadExact(f, data, ib);
            }
            posting_out_add(out, hash, root, data, ib);
        }
        for (i = 0; i < (int) surf_n; i++)
        {
            uint64      key;

            BufFileReadExact(f, &key, sizeof(key));
            posting_out_add_key(out, key);
        }
        BufFileClose(f);
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

    {
        int         r;

        for (r = 0; r < pgtshared->nranges; r++)
            tuplesort_attach_shared(RANGE_SORT(sharedsort, pgtshared->sortstride, r),
                                    seg);
    }
    SharedFileSetAttach(&pgtshared->resultfs, seg);
    sortmem = maintenance_work_mem / pgtshared->scantuplesortstates;

    /* Scan our heap slice into the range sorts, then build ranges. */
    pgtre_begin_range_sorts(&wstate, pgtshared, sharedsort, tc->lt_opr, sortmem);
    pgtre_scan_and_sort(&wstate, false);
    pgtre_finish_range_sorts(&wstate, pgtshared);
    pgtre_build_ranges(pgtshared, sharedsort, indexRel, tc->lt_opr);

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
    PostingOut  pout;
    BlockNumber root_upper;
    UpperIterState iter_state;
    PgTreLeader *build_leader = NULL;

    /* Phase 2 real build starts here. */

    /* Step 1: initialize empty meta page. */
    pg_tre_build_empty(index);

    /* Step 2: set up the disk-spillable sort and bloom tracking. */
    bstate.heap = heap;
    /*
     * A nondeterministic collation (e.g. ICU with strength=level1, where
     * 'a' = 'A' or 'é' = 'e') makes = and LIKE match strings that share no
     * trigrams, so no trigram index can find every match.  Refuse rather
     * than silently under-return, as core refuses ILIKE on such
     * collations.  rd_indcollation already reflects a column COLLATE, a
     * CREATE INDEX ... COLLATE override, or the database default.
     */
    if (OidIsValid(index->rd_indcollation[0]) &&
        !get_collation_isdeterministic(index->rd_indcollation[0]))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pg_tre does not support nondeterministic collations"),
                 errdetail("Index \"%s\" would use collation \"%s\", under which equal strings need not share trigrams.",
                           RelationGetRelationName(index),
                           get_collation_name(index->rd_indcollation[0])),
                 errhint("Use a deterministic collation for the indexed column, or index it with COLLATE \"C\".")));

    bstate.index = index;
    bstate.indexInfo = indexInfo;
    bstate.rsorts = NULL;
    bstate.nranges = 0;
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
             * Parallel build.  The leader participates as a worker: it
             * scans a heap slice into its own per-range worker-role sorts,
             * exactly like a background worker (mirrors nbtree's
             * _bt_leader_participate_as_worker).  The ranges' merges and
             * posting trees are built in Step 5 by every participant.
             */
            build_leader = leader;
            {
                BuildState  wstate;

                memset(&wstate, 0, sizeof(wstate));
                wstate.heap = heap;
                wstate.index = index;
                wstate.indexInfo = BuildIndexInfo(index);
                wstate.indexInfo->ii_Concurrent = indexInfo->ii_Concurrent;
                wstate.pgtshared = bstate.pgtshared;
                wstate.sharedsort = bstate.sharedsort;
                wstate.tmpctx = AllocSetContextCreate(CurrentMemoryContext,
                                                      "pg_tre leader-worker temp",
                                                      ALLOCSET_DEFAULT_SIZES);

                sortmem = maintenance_work_mem / leader->nparticipanttuplesorts;
                pgtre_begin_range_sorts(&wstate, bstate.pgtshared,
                                        bstate.sharedsort, tc->lt_opr, sortmem);
                pgtre_scan_and_sort(&wstate, true);
                pgtre_finish_range_sorts(&wstate, bstate.pgtshared);
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

    if (build_leader != NULL)
        pgtre_parallel_postings(build_leader, index, &pout);
    else
    {
        posting_out_init(&pout);
        build_postings(bstate.sortstate, index, &pout);
    }

    if (pout.n_skipped > 0)
        ereport(NOTICE,
                (errmsg("pg_tre: built %d posting trees "
                        "(%d trigrams skipped: below pg_tre.min_trigram_freq=%d)",
                        pout.n_accums, pout.n_skipped,
                        pg_tre_min_trigram_freq)));
    else
        ereport(NOTICE,
                (errmsg("pg_tre: built %d posting trees", pout.n_accums)));

    /* Step 6: bulk-load upper tree from the posting list. */
    iter_state.accums = pout.accums;
    iter_state.n_accums = pout.n_accums;
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

        uint64     *surf_keys = pout.surf_keys;
        uint32      surf_n = pout.surf_n;

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
                              (uint64) pout.n_accums, (uint64) bstate.heap_tuples);
        pg_tre_meta_set_surf(index, root_surf, surf_distinct);
    }

    MemoryContextSwitchTo(oldcxt);
    MemoryContextDelete(bstate.tmpctx);

    if (build_leader == NULL)
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
                    bstate.heap_tuples, pout.n_accums)));

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
    scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL
#if PG_VERSION_NUM >= 190000
                           , SO_NONE
#endif
                           );

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
