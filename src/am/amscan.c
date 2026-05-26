/*
 * src/am/amscan.c - scan path.
 *
 * Phase 3 (this file): exact-regex (k=0) scan.
 *
 *   ambeginscan:  allocate scan state (TreScanState) as scan->opaque.
 *   amrescan:     copy keys; decode tre_pattern from scan key's sk_argument;
 *                 parse regex; extract TrigramQuery; stash on scan state.
 *   amgetbitmap:  walk posting trees for each required trigram, AND the
 *                 results, emit to TIDBitmap via tbm_add_tuples.
 *   amendscan:    free scan state.
 *
 * Phase 5 extends amgetbitmap with tier-1 range bloom, tier-3 per-tuple
 * bloom, positional filtering, and k>0 via Navarro tiling.
 */

#include "postgres.h"

#include <limits.h>
#include <string.h>

#include "access/amapi.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "executor/tuptable.h"
#include "nodes/tidbitmap.h"
#include "storage/bufmgr.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "pg_tre/amapi.h"
#include "pg_tre/bloom.h"
#include "pg_tre/pattern_cache.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/page.h"
#include "pg_tre/pending.h"
#include "pg_tre/posting.h"
#include "pg_tre/range.h"
#include "pg_tre/regex_ast.h"
#include "pg_tre/sparsemap.h"
#include "pg_tre/tre_match.h"


/*
 * KNN heap entry: packed TID + computed edit distance.
 * Used for the index-side ORDER BY <@> path; see knn_build /
 * pg_tre_amgettuple below.
 */
typedef struct OrderEntry
{
    uint64  packed_tid;
    int32   dist;
} OrderEntry;

typedef struct TreScanState
{
    MemoryContext  scan_cxt;   /* per-scan memory context */
    TreParseCtx    parse_ctx;
    TrigramQuery   q;
    bool           query_valid;

    /*
     * KNN / ORDER BY <@> state.  Populated lazily on first amgettuple
     * call.  We compute the candidate sparsemap (same as amgetbitmap),
     * compute exact distance for each candidate by heap-fetching the
     * indexed column, sort by (distance ASC, packed_tid ASC) for
     * deterministic tie-breaking, and stream results in order.
     *
     * Why no online min-heap with early termination?  The pg_tre index
     * does not store the indexed text -- only trigrams and TIDs.  We
     * have no per-row distance lower bound from the index alone, so
     * the closest unexamined candidate could have distance 0.  Any
     * "streaming" approach would still have to examine every
     * candidate before the first emit can be proven safe.  Pre-sorting
     * is functionally identical and simpler than the equivalent heap
     * drain.  Early termination at LIMIT is a property of the executor:
     * once the LIMIT node has its N rows it stops calling amgettuple,
     * leaving any tail of the sorted array untouched.
     */
    bool             knn_ready;
    OrderEntry      *knn_entries;
    int              knn_n;
    int              knn_pos;
    void            *knn_compiled;   /* tre_cache_lookup() result */
    int32            knn_max_cost;
    AttrNumber       knn_body_attno;
} TreScanState;

IndexScanDesc
pg_tre_ambeginscan(Relation index, int nkeys, int norderbys)
{
    IndexScanDesc  scan = RelationGetIndexScan(index, nkeys, norderbys);
    TreScanState  *st;

    st = (TreScanState *) palloc0(sizeof(*st));
    st->scan_cxt = AllocSetContextCreate(CurrentMemoryContext,
                                         "pg_tre scan",
                                         ALLOCSET_DEFAULT_SIZES);
    st->query_valid = false;
    st->knn_ready = false;
    st->knn_entries = NULL;
    st->knn_n = 0;
    st->knn_pos = 0;
    st->knn_compiled = NULL;
    st->knn_body_attno = InvalidAttrNumber;

    /*
     * RelationGetIndexScan does NOT allocate xs_orderbyvals /
     * xs_orderbynulls in PG18; the AM is expected to do it (see
     * gistscan.c / spgscan.c for the pattern).  Without these
     * allocations, scan->xs_orderbyvals points at randomized
     * memory in --enable-cassert builds and the executor will
     * crash on the first KNN result.
     */
    if (norderbys > 0)
    {
        scan->xs_orderbyvals  = palloc0(sizeof(Datum) * norderbys);
        scan->xs_orderbynulls = palloc(sizeof(bool) * norderbys);
        memset(scan->xs_orderbynulls, true, sizeof(bool) * norderbys);
    }
    scan->opaque = st;
    return scan;
}

void
pg_tre_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                ScanKey orderbys, int norderbys)
{
    TreScanState *st = (TreScanState *) scan->opaque;
    MemoryContext old;
    ScanKey        sk = NULL;
    struct TrePatternData *pat;
    char          *pattern_str;
    int            pattern_len;
    int32          max_cost;

    if (keys && scan->numberOfKeys > 0)
        memcpy(scan->keyData, keys,
               scan->numberOfKeys * sizeof(ScanKeyData));
    if (orderbys && scan->numberOfOrderBys > 0)
        memcpy(scan->orderByData, orderbys,
               scan->numberOfOrderBys * sizeof(ScanKeyData));

    /* Reset per-scan context. */
    MemoryContextReset(st->scan_cxt);
    st->query_valid = false;
    /* Reset KNN state (memory was in scan_cxt, just freed). */
    st->knn_ready = false;
    st->knn_entries = NULL;
    st->knn_n = 0;
    st->knn_pos = 0;
    st->knn_compiled = NULL;
    st->knn_body_attno = InvalidAttrNumber;

    if (scan->numberOfKeys < 1 && scan->numberOfOrderBys < 1)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pg_tre: scan requires at least one key or ORDER BY")));

    /*
     * Pick the pattern source.  Prefer the WHERE clause's %~~ argument
     * (which determines the candidate set) over the ORDER BY <@>
     * argument.  When there is no WHERE clause, fall back to ORDER BY
     * so the planner can drive a pure KNN scan.
     */
    if (scan->numberOfKeys >= 1)
    {
        sk = &scan->keyData[0];
        if (sk->sk_strategy != PG_TRE_STRATEGY_APPROX_MATCH)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("pg_tre: unsupported scan strategy %d",
                            sk->sk_strategy)));
    }
    else
    {
        sk = &scan->orderByData[0];
        if (sk->sk_strategy != PG_TRE_STRATEGY_DISTANCE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("pg_tre: unsupported ORDER BY strategy %d",
                            sk->sk_strategy)));
    }

    old = MemoryContextSwitchTo(st->scan_cxt);

    pat = (struct TrePatternData *) DatumGetPointer(sk->sk_argument);
    pattern_str = tre_pattern_get_text(pat, &pattern_len);
    max_cost = tre_pattern_get_max_cost(pat);

    /* Phase 5: k > 0 is now supported via tiling + three-tier filtering.
     * Extraction may still return always_true if the pattern defeats tiling
     * (too short, no literal runs, etc.); amgetbitmap handles that case. */

    /* Parse regex into AST. */
    if (!tre_parse_regex(&st->parse_ctx, pattern_str, pattern_len))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
                 errmsg("pg_tre: invalid regex pattern: %s",
                        st->parse_ctx.errmsg)));

    /* Extract trigram query (now handles k > 0 via tiling). */
    if (!regex_extract_query(&st->parse_ctx, max_cost, &st->q))
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_tre: trigram extraction failed")));

    st->query_valid = true;
    MemoryContextSwitchTo(old);
}

/*
 * Pending-list overlay: for each trigram that appears in the query,
 * collect the set of TIDs referenced in the pending list.  This map
 * is built once per scan, then OR-ed into each disjunct's posting
 * during resolve_conjunct below.
 */
/*
 * Pending-list overlay: for each trigram in the query, collect the set
 * of TIDs found in the pending list.  We collect into a palloc'd uint64
 * array (resilient under repeated grow/shrink) and lazily convert to a
 * sparsemap on first lookup.  An earlier implementation used
 * sm_set_data_size to dynamically grow a calloc-backed sparsemap
 * during collection, which under high-volume pending-list scans hit a
 * heap-corruption path inside the realloc.  See tap/concurrency.pl
 * regression for the gate.
 */
typedef struct PendingOverlayEntry
{
    uint64       trigram_hash;
    uint64      *tids_arr;     /* palloc'd, sorted by insertion order */
    int          tids_n;
    int          tids_cap;
    sparsemap_t *tids;         /* lazily built on first overlay_lookup */
} PendingOverlayEntry;

typedef struct PendingOverlay
{
    PendingOverlayEntry *entries;
    int                  n;
    int                  alloced;
    MemoryContext        mcxt;
    /* Fast look-up bloom: which trigram_hashes appeared in the query? */
    const TrigramQuery  *q;
} PendingOverlay;

static bool
query_wants_trigram(const TrigramQuery *q, uint64 h)
{
    int i, j;
    for (i = 0; i < q->n; i++)
    {
        const TrigramConjunct *c = &q->conjuncts[i];
        for (j = 0; j < c->n; j++)
            if (c->alts[j].trigram_hash == h)
                return true;
    }
    return false;
}

/*
 * True if the query reduces to a single trigram (one conjunct with one
 * disjunct).  In that case the candidate set IS the posting list of
 * that trigram, and tier-3 / positional filters cannot prune anything:
 * every candidate's per-tuple bloom contains the trigram by
 * construction (see ambuild.c::find_or_create_tid_bloom), and the only
 * disjunct's position window covers the query's full match span.
 * Skipping tier-3 here turns a multi-minute hang into a millisecond
 * pass-through; correctness is preserved by the executor recheck.
 */
static bool
tre_query_is_single_trigram(const TrigramQuery *q)
{
    return q->n == 1 && q->conjuncts[0].n == 1;
}

static PendingOverlayEntry *
overlay_find_or_create(PendingOverlay *ov, uint64 h)
{
    int i;
    MemoryContext old;
    PendingOverlayEntry *e;

    for (i = 0; i < ov->n; i++)
        if (ov->entries[i].trigram_hash == h)
            return &ov->entries[i];

    old = MemoryContextSwitchTo(ov->mcxt);
    if (ov->n >= ov->alloced)
    {
        ov->alloced = (ov->alloced == 0) ? 16 : ov->alloced * 2;
        ov->entries = ov->entries
            ? repalloc(ov->entries, ov->alloced * sizeof(*ov->entries))
            : palloc(ov->alloced * sizeof(*ov->entries));
    }

    e = &ov->entries[ov->n++];
    e->trigram_hash = h;
    e->tids_arr     = palloc(sizeof(uint64) * 64);
    e->tids_n       = 0;
    e->tids_cap     = 64;
    e->tids         = NULL;
    MemoryContextSwitchTo(old);
    return e;
}

static void
overlay_collect_cb(uint64 hash, ItemPointer tid, uint32 position, void *ctx)
{
    PendingOverlay *ov = ctx;
    PendingOverlayEntry *e;
    uint64 packed;

    (void) position;

    if (!query_wants_trigram(ov->q, hash))
        return;

    e = overlay_find_or_create(ov, hash);
    packed = pg_tre_pack_tid(tid);

    if (e->tids_n >= e->tids_cap)
    {
        MemoryContext old = MemoryContextSwitchTo(ov->mcxt);
        e->tids_cap *= 2;
        e->tids_arr = repalloc(e->tids_arr,
                               sizeof(uint64) * e->tids_cap);
        MemoryContextSwitchTo(old);
    }
    e->tids_arr[e->tids_n++] = packed;
}

static void
overlay_build(PendingOverlay *ov, Relation index, const TrigramQuery *q,
              MemoryContext mcxt)
{
    memset(ov, 0, sizeof(*ov));
    ov->mcxt = mcxt;
    ov->q    = q;
    pg_tre_pending_scan(index, overlay_collect_cb, ov);
}

static int
overlay_uint64_cmp(const void *a, const void *b)
{
    uint64 x = *(const uint64 *) a;
    uint64 y = *(const uint64 *) b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static sparsemap_t *
overlay_lookup(const PendingOverlay *ov, uint64 h)
{
    int i, k;
    PendingOverlayEntry *e;
    sparsemap_t *sm;

    for (i = 0; i < ov->n; i++)
    {
        if (ov->entries[i].trigram_hash != h)
            continue;

        e = (PendingOverlayEntry *) &ov->entries[i];
        if (e->tids != NULL)
            return e->tids;

        if (e->tids_n == 0)
            return NULL;

        /* Lazy materialise: sort + dedupe + build a sparsemap of the
         * exact size needed.  Avoids the dynamic-grow path in
         * sm_set_data_size that triggered heap corruption
         * under heavy concurrent insert load (caught by
         * tap/concurrency.pl). */
        qsort(e->tids_arr, e->tids_n, sizeof(uint64), overlay_uint64_cmp);

        sm = sm_create(16384);  /* Start with larger capacity to reduce grows */
        if (sm == NULL)
            return NULL;

        for (k = 0; k < e->tids_n; k++)
        {
            uint64 packed = e->tids_arr[k];
            uint64 rc;

            if (k > 0 && packed == e->tids_arr[k - 1])
                continue;  /* dedupe */

            /*
             * sm_add_grow geometrically doubles the buffer when sm_add
             * would have returned SM_IDX_MAX, so the only failure mode
             * left here is allocation failure.
             */
            rc = sm_add_grow(&sm, packed);
            if (rc == SM_IDX_MAX)
            {
                sm_free(sm);
                return NULL;
            }
        }
        e->tids = sm;
        return sm;
    }
    return NULL;
}

static void
overlay_free(PendingOverlay *ov)
{
    int i;
    for (i = 0; i < ov->n; i++)
    {
        if (ov->entries[i].tids)
            free(ov->entries[i].tids);
        /* tids_arr is palloc'd in ov->mcxt and freed when context resets. */
    }
}

/*
 * Build the candidate TID sparsemap for one conjunct: OR of its
 * disjuncts, including the pending-list overlay for each trigram.
 */
static sparsemap_t *
resolve_conjunct_with_overlay(Relation index, const TrigramConjunct *conj,
                              MemoryContext cxt, const PendingOverlay *ov)
{
    sparsemap_t *accum = NULL;
    int i;

    for (i = 0; i < conj->n; i++)
    {
        PgTreUpperRef ref;
        sparsemap_t  *sm = NULL;
        sparsemap_t  *pend;

        if (pg_tre_upper_lookup(index, conj->alts[i].trigram_hash, &ref))
        {
            sm = pg_tre_posting_materialize(index, ref.root,
                                            ref.inline_data,
                                            ref.inline_bytes);
            pg_tre_upper_release(&ref);
        }

        pend = overlay_lookup(ov, conj->alts[i].trigram_hash);
        if (pend != NULL)
        {
            if (sm == NULL)
            {
                sm = sm_copy(pend);
            }
            else
            {
                sparsemap_t *u = sm_union(sm, pend);
                free(sm);
                sm = u;
            }
        }

        if (sm == NULL)
            continue;   /* neither index nor pending has this trigram */

        if (accum == NULL)
        {
            accum = sm;
        }
        else
        {
            sparsemap_t *merged = sm_union(accum, sm);
            free(accum);
            free(sm);
            accum = merged;
        }
    }

    (void) cxt;
    return accum;
}

/*
 * Scanner callback: reserved for a future fast-path using sm_scan
 * (which emits 64-bit vectors).  Phase 3 walks set bits via
 * sm_contains instead; the callback version becomes useful in
 * Phase 5 when we batch TIDs through tier-3 bloom refinement before
 * emitting.
 */
#if 0
typedef struct ScanCbCtx
{
    TIDBitmap   *tbm;
    int64        count;
} ScanCbCtx;

static void
sm_scan_cb(uint64 vec[], size_t n, void *aux)
{
    (void) vec;
    (void) n;
    (void) aux;
}
#endif

/*
 * Tier-3 per-tuple bloom filter: refine a candidate TID sparsemap by
 * checking each TID's per-tuple bloom against all required trigrams.
 * Returns a new sparsemap containing only TIDs that pass the bloom filter.
 *
 * Phase 5 stub: pg_tre_posting_lookup_tuple_bloom is implemented by
 * Phase 5 WRITE, but returns false for TIDs without payload (Phase 4
 * postings).  So this function gracefully degrades to a no-op for indexes
 * built before Phase 5 payload support.
 */
static sparsemap_t *
apply_tuple_bloom_filter(Relation index, const TrigramQuery *q,
                        sparsemap_t *candidates, MemoryContext cxt)
{
    sparsemap_t *refined;
    uint64 idx;
    /*
     * Bloom scratch buffer.  Layout: [PgTreBloom header][bit array].
     * The buffer is sized to fit any bloom width the index could
     * surface: max(GUC pg_tre.bloom_tuple_bits, PG_TRE_BLOOM_MAX_M_BITS)
     * bits.  The GUC currently caps at 1024 bits (128 B);
     * PG_TRE_BLOOM_MAX_M_BITS is 512 (64 B); 256 bytes is plenty.
     *
     * Pre-1.2.1 this code cast a raw bit array directly to
     * (PgTreBloom *) and read garbage from the bit data as if it
     * were the m_bits/k header - the chain-rank "bug" we tracked
     * for several releases was actually this.  Tier-3 was gated
     * off via pg_tre.tuple_bloom_enable=false to mask it.
     */
    uint8 bloom_buf[256];
    Size bit_array_capacity;
    int i, j;

    if (candidates == NULL || sm_cardinality(candidates) == 0)
        return candidates;

    bit_array_capacity = sizeof(bloom_buf) - MAXALIGN(sizeof(PgTreBloom));

    refined = sm_create(sm_get_capacity(candidates) * 2 + 256);
    if (refined == NULL)
        return candidates;  /* OOM; fall back to unrefined */

    /*
     * Iterate set bits via sm_next_member rather than walking
     * [sm_minimum, sm_maximum] and calling sm_contains at every
     * gap.  For a 100K-row 1000-page heap the old walk visited
     * ~65M empty indexes; this one visits only candidates.
     */
    idx = SM_IDX_MAX;
    while ((idx = sm_next_member(candidates, idx)) != SM_IDX_MAX)
    {
        bool passes = true;

            /* For each conjunct, check if at least one disjunct's trigram
             * is present in the tuple's bloom.  In CNF mode, ALL conjuncts
             * must have at least one disjunct present.  In DNF mode, at
             * least ONE conjunct must have ALL its disjuncts present. */

            if (q->mode == TRIGRAM_QUERY_CNF)
            {
                /* CNF: AND across conjuncts */
                for (i = 0; i < q->n && passes; i++)
                {
                    const TrigramConjunct *conj = &q->conjuncts[i];
                    bool conj_pass = false;
                    /* Header lives at offset 0; bit array starts at
                     * MAXALIGN(sizeof(PgTreBloom)).  See the
                     * bloom_buf comment above. */
                    PgTreBloom *bloom_view = (PgTreBloom *) bloom_buf;
                    uint8 *bit_array = bloom_buf + MAXALIGN(sizeof(PgTreBloom));

                    for (j = 0; j < conj->n; j++)
                    {
                        uint64 h = conj->alts[j].trigram_hash;
                        PgTreUpperRef ref;
                        bool has_bloom = false;
                        uint32 page_fmt = PG_TRE_FORMAT_VERSION_LATEST;
                        uint16 m_bits = 0;
                        uint8  k = 0;

                        if (pg_tre_upper_lookup(index, h, &ref))
                        {
                            has_bloom = pg_tre_posting_lookup_tuple_bloom(
                                            index, ref.root, ref.inline_data,
                                            ref.inline_bytes, idx,
                                            bit_array, bit_array_capacity,
                                            &m_bits, &k, &page_fmt);
                            pg_tre_upper_release(&ref);
                        }

                        if (has_bloom)
                        {
                            /* Reconstruct the bloom header from the
                             * (m_bits, k) the page-walker surfaced --
                             * v6 from the on-disk header, pre-v6 from
                             * the index-wide GUC. */
                            if (!pg_tre_bloom_decode_tuple(bloom_view,
                                                          m_bits, k,
                                                          page_fmt))
                            {
                                /* Unknown format -- fall back to
                                 * recheck rather than reject. */
                                conj_pass = true;
                                break;
                            }
                            if (pg_tre_bloom_contains_trigram(bloom_view, h))
                            {
                                conj_pass = true;
                                break;
                            }
                        }
                        else
                        {
                            /* No bloom data (Phase 4 posting or inline);
                             * conservatively assume the trigram might be
                             * present (pass through). */
                            conj_pass = true;
                            break;
                        }
                    }

                    if (!conj_pass)
                        passes = false;
                }
            }
            else
            {
                /* DNF: OR across conjuncts (tiles); at least one tile
                 * must have ALL its trigrams present. */
                passes = false;
                for (i = 0; i < q->n && !passes; i++)
                {
                    const TrigramConjunct *tile = &q->conjuncts[i];
                    bool tile_pass = true;
                    PgTreBloom *bloom_view = (PgTreBloom *) bloom_buf;
                    uint8 *bit_array = bloom_buf + MAXALIGN(sizeof(PgTreBloom));

                    for (j = 0; j < tile->n && tile_pass; j++)
                    {
                        uint64 h = tile->alts[j].trigram_hash;
                        PgTreUpperRef ref;
                        bool has_bloom = false;
                        uint32 page_fmt = PG_TRE_FORMAT_VERSION_LATEST;
                        uint16 m_bits = 0;
                        uint8  k = 0;

                        if (pg_tre_upper_lookup(index, h, &ref))
                        {
                            has_bloom = pg_tre_posting_lookup_tuple_bloom(
                                            index, ref.root, ref.inline_data,
                                            ref.inline_bytes, idx,
                                            bit_array, bit_array_capacity,
                                            &m_bits, &k, &page_fmt);
                            pg_tre_upper_release(&ref);
                        }

                        if (has_bloom)
                        {
                            /* Format-version-aware decode.  See the
                             * CNF arm above for rationale. */
                            if (!pg_tre_bloom_decode_tuple(bloom_view,
                                                          m_bits, k,
                                                          page_fmt))
                            {
                                /* Unknown format -- skip this tile. */
                                continue;
                            }
                            if (!pg_tre_bloom_contains_trigram(bloom_view, h))
                            {
                                tile_pass = false;
                                break;
                            }
                        }
                        else
                        {
                            /* No bloom; assume pass */
                        }
                    }

                    if (tile_pass)
                        passes = true;
                }
            }

            if (passes)
            {
                if (sm_add_grow(&refined, idx) == SM_IDX_MAX)
                {
                    /* Out of memory; bail to unrefined result. */
                    sm_free(refined);
                    return candidates;
                }
            }
    }

    (void) cxt;
    free(candidates);
    return refined;
}

/*
 * Compute the candidate-TID sparsemap for the current scan.
 *
 * Caller must have st->scan_cxt as the current memory context.  The
 * returned sparsemap is malloc'd (not palloc'd); caller frees it via
 * free().  Returns NULL when the result is empty OR when the query is
 * always_true (in the latter case *out_always_true is set).
 */
static sparsemap_t *
tre_compute_candidate_sm(IndexScanDesc scan, TreScanState *st,
                         bool *out_always_true)
{
    sparsemap_t  *result = NULL;
    int           i;

    *out_always_true = false;

    if (st->q.always_true)
    {
        *out_always_true = true;
        return NULL;
    }

    /*
     * Build the pending-list overlay once.  If the index has no
     * pending entries, this is a single metapage read and returns
     * immediately.
     */
    {
        PendingOverlay ov;
        overlay_build(&ov, scan->indexRelation, &st->q, st->scan_cxt);

        if (st->q.mode == TRIGRAM_QUERY_CNF)
        {
            /* CNF: AND across conjuncts. */
            for (i = 0; i < st->q.n; i++)
            {
                sparsemap_t *sm = resolve_conjunct_with_overlay(
                                      scan->indexRelation,
                                      &st->q.conjuncts[i],
                                      st->scan_cxt, &ov);
                if (sm == NULL)
                {
                    if (result != NULL) free(result);
                    overlay_free(&ov);
                    return NULL;
                }

                if (result == NULL)
                {
                    result = sm;
                }
                else
                {
                    sparsemap_t *merged = sm_intersection(result, sm);
                    free(result);
                    free(sm);
                    result = merged;
                    if (result == NULL ||
                        (sm_get_size(result) != 0 &&
                         sm_cardinality(result) == 0))
                    {
                        if (result) free(result);
                        overlay_free(&ov);
                        return NULL;
                    }
                }
            }
        }
        else  /* TRIGRAM_QUERY_DNF */
        {
            for (i = 0; i < st->q.n; i++)
            {
                const TrigramConjunct *tile = &st->q.conjuncts[i];
                int j;

                for (j = 0; j < tile->n; j++)
                {
                    PgTreUpperRef ref;
                    sparsemap_t *sm = NULL;
                    sparsemap_t *pend;

                    if (pg_tre_upper_lookup(scan->indexRelation,
                                           tile->alts[j].trigram_hash, &ref))
                    {
                        sm = pg_tre_posting_materialize(scan->indexRelation,
                                                       ref.root,
                                                       ref.inline_data,
                                                       ref.inline_bytes);
                        pg_tre_upper_release(&ref);
                    }

                    pend = overlay_lookup(&ov, tile->alts[j].trigram_hash);
                    if (pend != NULL)
                    {
                        if (sm == NULL)
                            sm = sm_copy(pend);
                        else
                        {
                            sparsemap_t *u = sm_union(sm, pend);
                            free(sm);
                            sm = u;
                        }
                    }

                    if (sm == NULL)
                        continue;

                    if (result == NULL)
                        result = sm;
                    else
                    {
                        sparsemap_t *merged = sm_union(result, sm);
                        free(result);
                        free(sm);
                        result = merged;
                    }
                }
            }
        }

        overlay_free(&ov);
    }

    /* Tier-3: per-tuple bloom filter (chain-rank gated; see comment in
     * pg_tre_amgetbitmap below). */
    if (result != NULL && st->q.global_max_cost >= 0 &&
        pg_tre_tuple_bloom_enable &&
        !tre_query_is_single_trigram(&st->q) &&
        sm_cardinality(result) <= (uint64) pg_tre_tier3_max_candidates)
    {
        result = apply_tuple_bloom_filter(scan->indexRelation, &st->q,
                                          result, st->scan_cxt);
    }

    /* Phase 5.1: positional filter (CNF only). */
    if (result != NULL && sm_cardinality(result) > 0 &&
        st->q.mode == TRIGRAM_QUERY_CNF &&
        pg_tre_tuple_bloom_enable &&
        !tre_query_is_single_trigram(&st->q) &&
        sm_cardinality(result) <= (uint64) pg_tre_tier3_max_candidates)
    {
        sparsemap_t *filtered = sm_create(sm_get_capacity(result));
        if (filtered != NULL)
        {
            uint64 tid_idx = SM_IDX_MAX;

            while ((tid_idx = sm_next_member(result, tid_idx)) != SM_IDX_MAX)
            {
                bool passes = (st->q.mode == TRIGRAM_QUERY_CNF);
                int ci, j;

                for (ci = 0;
                     ci < st->q.n &&
                     (st->q.mode == TRIGRAM_QUERY_CNF ? passes : !passes);
                     ci++)
                {
                    const TrigramConjunct *conj = &st->q.conjuncts[ci];

                    if (st->q.mode == TRIGRAM_QUERY_CNF)
                    {
                        bool conj_pass = false;
                        bool any_evaluated = false;
                        for (j = 0; j < conj->n && !conj_pass; j++)
                        {
                            const TrigramDisjunct *dis = &conj->alts[j];
                            PgTreUpperRef ref;
                            const uint32 *positions;
                            int n_positions, p;

                            if (!pg_tre_upper_lookup(scan->indexRelation,
                                                    dis->trigram_hash, &ref))
                                continue;
                            any_evaluated = true;

                            n_positions = pg_tre_posting_lookup_positions(
                                            scan->indexRelation,
                                            ref.root, ref.inline_data,
                                            ref.inline_bytes,
                                            tid_idx, &positions);
                            pg_tre_upper_release(&ref);

                            if (n_positions == 0)
                            {
                                conj_pass = true;
                                break;
                            }

                            for (p = 0; p < n_positions; p++)
                            {
                                if (positions[p] >= (uint32) dis->min_offset &&
                                    positions[p] <= (uint32) dis->max_offset)
                                {
                                    conj_pass = true;
                                    break;
                                }
                            }
                        }
                        if (!any_evaluated)
                            conj_pass = true;
                        if (!conj_pass)
                            passes = false;
                    }
                    else  /* DNF */
                    {
                        bool tile_pass = true;
                        for (j = 0; j < conj->n && tile_pass; j++)
                        {
                            const TrigramDisjunct *dis = &conj->alts[j];
                            PgTreUpperRef ref;
                            const uint32 *positions;
                            int n_positions, p;
                            bool this_tri_pass = false;

                            if (!pg_tre_upper_lookup(scan->indexRelation,
                                                    dis->trigram_hash, &ref))
                            {
                                tile_pass = false;
                                break;
                            }

                            n_positions = pg_tre_posting_lookup_positions(
                                            scan->indexRelation,
                                            ref.root, ref.inline_data,
                                            ref.inline_bytes,
                                            tid_idx, &positions);
                            pg_tre_upper_release(&ref);

                            if (n_positions == 0)
                                this_tri_pass = true;
                            else
                            {
                                for (p = 0; p < n_positions; p++)
                                {
                                    if (positions[p] >= (uint32) dis->min_offset &&
                                        positions[p] <= (uint32) dis->max_offset)
                                    {
                                        this_tri_pass = true;
                                        break;
                                    }
                                }
                            }

                            if (!this_tri_pass)
                                tile_pass = false;
                        }

                        if (tile_pass)
                        {
                            passes = true;
                            break;
                        }
                        else
                            passes = false;
                    }
                }

                if (passes)
                {
                    if (sm_add(filtered, tid_idx) == SM_IDX_MAX)
                    {
                        free(filtered);
                        filtered = NULL;
                        break;
                    }
                }
            }

            if (filtered != NULL)
            {
                free(result);
                result = filtered;
            }
        }
    }

    return result;
}

int64
pg_tre_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
    TreScanState *st = (TreScanState *) scan->opaque;
    MemoryContext old;
    sparsemap_t  *result;
    bool          always_true = false;
    int64         ntids = 0;

    if (!st->query_valid)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_tre: amgetbitmap called without amrescan")));

    /*
     * If extraction gave up (always_true), the index has no useful
     * filter information for this pattern.  Emit a fully-lossy
     * TIDBitmap covering every block of the heap; the executor's
     * recheck (tre_match_scalar -> pg_tre_amatch) will discard
     * non-matches.  Performance degrades to a sequential scan but
     * correctness is preserved.
     */
    if (st->q.always_true)
    {
        Relation   heap = scan->heapRelation;
        BlockNumber  nblocks;
        BlockNumber  blk;
        int64        emitted = 0;

        if (heap == NULL)
            heap = table_open(scan->indexRelation->rd_index->indrelid,
                              AccessShareLock);

        nblocks = RelationGetNumberOfBlocks(heap);
        for (blk = 0; blk < nblocks; blk++)
        {
            tbm_add_page(tbm, blk);
            emitted++;
        }

        if (heap != scan->heapRelation)
            table_close(heap, AccessShareLock);

        ereport(DEBUG1,
                (errmsg("pg_tre: extraction always_true; emitted lossy "
                        "bitmap covering %ld blocks (recheck will filter)",
                        (long) emitted)));
        return emitted;
    }

    old = MemoryContextSwitchTo(st->scan_cxt);
    result = tre_compute_candidate_sm(scan, st, &always_true);

    if (result != NULL)
    {
        if (sm_cardinality(result) > 0)
        {
            uint64 i = SM_IDX_MAX;
            while ((i = sm_next_member(result, i)) != SM_IDX_MAX)
            {
                ItemPointerData tid;
                pg_tre_unpack_tid(i, &tid);
                tbm_add_tuples(tbm, &tid, 1, true /* lossy */);
                ntids++;
            }
        }

        free(result);
    }

    MemoryContextSwitchTo(old);
    return ntids;
}

void
pg_tre_amendscan(IndexScanDesc scan)
{
    TreScanState *st = (TreScanState *) scan->opaque;
    if (st == NULL)
        return;
    MemoryContextDelete(st->scan_cxt);
    pfree(st);
    scan->opaque = NULL;
}

/* ------------------------------------------------------------------
 * KNN / ORDER BY <@>: amgettuple implementation.
 *
 * The planner picks this path when the index is asked to return rows
 * sorted by an ordering operator whose strategy is registered in the
 * opclass (here, strategy 2 = `<@>` -> tre_distance).  See amapi.c
 * (amcanorderbyop = true) and the ORDER BY clause in the
 * tre_text_ops opclass for the catalog wiring.
 *
 * Algorithm:
 *   1. On first call after amrescan, build the candidate sparsemap
 *      (same logic as amgetbitmap).
 *   2. For each candidate TID, fetch the heap tuple with the scan
 *      snapshot, extract the indexed text, compute the exact edit
 *      distance.  Skip TIDs that are not visible to the snapshot
 *      (the executor would have skipped them too).
 *   3. Sort by (distance ASC, packed_tid ASC).  Tie-breaking on the
 *      TID gives a stable, repeatable ordering for tests.
 *   4. Stream entries one at a time on subsequent calls.
 *
 * Recheck flags:
 *   xs_recheck = true        -- the index uses approximate filtering
 *                              (trigram + bloom + positional), so the
 *                              executor must recompute body %~~ pat.
 *   xs_recheckorderby = false -- the distance we returned is exact;
 *                              no Sort is needed above us.
 *
 * Why no streaming online min-heap?  See the design comment on
 * TreScanState.  Without an index-side distance lower bound, every
 * candidate must be examined before the first emit can be safely
 * proven minimum.  Pre-sorting is functionally identical and simpler.
 * The win over the existing executor sort is structural, not
 * algorithmic: pg_tre owns the distance computation, eliminates the
 * Sort node, and lets the LIMIT node terminate the scan after N
 * results without paying the full sort.
 * ------------------------------------------------------------------ */

static int
knn_entry_cmp(const void *a, const void *b)
{
    const OrderEntry *ea = a;
    const OrderEntry *eb = b;

    if (ea->dist != eb->dist)
        return (ea->dist < eb->dist) ? -1 : 1;
    if (ea->packed_tid < eb->packed_tid) return -1;
    if (ea->packed_tid > eb->packed_tid) return 1;
    return 0;
}

/*
 * Locate the heap attribute number for the indexed column.  pg_tre is
 * single-column, so we always look at index attribute 1.  indkey holds
 * the heap attribute numbers for each indexed column.
 */
static AttrNumber
resolve_body_attno(Relation indexRel)
{
    if (indexRel->rd_index == NULL ||
        indexRel->rd_index->indnatts < 1)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_tre: index has no indexed attributes")));
    return indexRel->rd_index->indkey.values[0];
}

static void
knn_build(IndexScanDesc scan, TreScanState *st)
{
    MemoryContext old;
    sparsemap_t   *result;
    bool          always_true = false;
    Relation      heap = scan->heapRelation;
    bool          opened_heap = false;
    AttrNumber    body_attno;
    void         *compiled = NULL;
    int32         max_cost = 0;
    struct TrePatternData *pat;
    char         *pat_text;
    int           pat_len;
    int           cap = 64;
    int           n_entries = 0;
    OrderEntry   *entries;
    bool          have_orderby = (scan->numberOfOrderBys >= 1 &&
                                  scan->orderByData != NULL);

    if (!st->query_valid)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_tre: amgettuple called without amrescan")));

    /*
     * Distance pattern: only relevant when an ORDER BY operator is
     * present.  When the planner picks Index Scan as a non-KNN path
     * (no ORDER BY), we still emit candidate TIDs but skip the
     * distance pipeline entirely; entries[].dist stays 0 and
     * xs_orderbyvals stays untouched.
     */
    if (have_orderby)
    {
        pat = (struct TrePatternData *)
              PG_DETOAST_DATUM(scan->orderByData[0].sk_argument);
        pat_text = tre_pattern_get_text(pat, &pat_len);
        max_cost = tre_pattern_get_max_cost(pat);
        compiled = tre_cache_lookup(pat_text, pat_len);
    }

    body_attno = resolve_body_attno(scan->indexRelation);

    if (heap == NULL)
    {
        heap = table_open(scan->indexRelation->rd_index->indrelid,
                          AccessShareLock);
        opened_heap = true;
    }

    old = MemoryContextSwitchTo(st->scan_cxt);
    entries = palloc(sizeof(*entries) * cap);

    result = tre_compute_candidate_sm(scan, st, &always_true);

    if (always_true)
    {
        /*
         * No candidate filter from the index.  We must consider every
         * heap row.  This degrades to a sequential scan but preserves
         * correctness; the cost estimator should already have steered
         * the planner away when this can be avoided.  Stream every
         * row's TID through the same distance pipeline.
         *
         * When there is no ORDER BY (have_orderby == false), we still
         * must run the executor's recheck path, so emit every TID and
         * let the caller's xs_recheck decide.
         */
        TableScanDesc heapscan;
        HeapTuple     htup;
        TupleDesc     desc = RelationGetDescr(heap);

        heapscan = table_beginscan_strat(heap, scan->xs_snapshot, 0, NULL,
                                         true /* allow_strat */, true /* allow_sync */);
        while ((htup = heap_getnext(heapscan, ForwardScanDirection)) != NULL)
        {
            bool      isnull = false;
            Datum     val;
            text     *body;
            int32     dist = 0;
            ItemPointerData tid;

            if (have_orderby)
            {
                TreMatchResult r;

                val = heap_getattr(htup, body_attno, desc, &isnull);
                if (isnull)
                    continue;
                body = (text *) PG_DETOAST_DATUM_PACKED(val);
                r = tre_do_match(compiled,
                                 VARDATA_ANY(body), VARSIZE_ANY_EXHDR(body),
                                 max_cost, 1, 1, 1,
                                 INT_MAX, INT_MAX, INT_MAX, INT_MAX);
                if (!r.matched)
                    continue;       /* NULLS LAST: drop no-match rows */
                dist = r.cost;
            }

            ItemPointerCopy(&htup->t_self, &tid);

            if (n_entries >= cap)
            {
                cap *= 2;
                entries = repalloc(entries, sizeof(*entries) * cap);
            }
            entries[n_entries].packed_tid = pg_tre_pack_tid(&tid);
            entries[n_entries].dist = dist;
            n_entries++;
        }
        table_endscan(heapscan);
    }
    else if (result != NULL && sm_cardinality(result) > 0)
    {
        uint64 idx = SM_IDX_MAX;

        while ((idx = sm_next_member(result, idx)) != SM_IDX_MAX)
        {
            ItemPointerData tid;
            int32           dist = 0;

            pg_tre_unpack_tid(idx, &tid);

            if (have_orderby)
            {
                HeapTupleData   htup;
                Buffer          buf = InvalidBuffer;
                TupleDesc       desc;
                bool            isnull = false;
                Datum           val;
                text           *body;
                TreMatchResult  r;

                ItemPointerCopy(&tid, &htup.t_self);

                if (!heap_fetch(heap, scan->xs_snapshot, &htup, &buf, false))
                {
                    if (BufferIsValid(buf))
                        ReleaseBuffer(buf);
                    continue;       /* not visible; executor would skip too */
                }

                desc = RelationGetDescr(heap);
                val = heap_getattr(&htup, body_attno, desc, &isnull);
                if (isnull)
                {
                    ReleaseBuffer(buf);
                    continue;
                }
                body = (text *) PG_DETOAST_DATUM_PACKED(val);
                r = tre_do_match(compiled,
                                 VARDATA_ANY(body), VARSIZE_ANY_EXHDR(body),
                                 max_cost, 1, 1, 1,
                                 INT_MAX, INT_MAX, INT_MAX, INT_MAX);
                ReleaseBuffer(buf);

                if (!r.matched)
                    continue;       /* false positive from trigram filter */
                dist = r.cost;
            }

            if (n_entries >= cap)
            {
                cap *= 2;
                entries = repalloc(entries, sizeof(*entries) * cap);
            }
            entries[n_entries].packed_tid = idx;
            entries[n_entries].dist = dist;
            n_entries++;
        }
    }

    if (result != NULL)
        free(result);

    /*
     * Sorting is only meaningful when ORDER BY is asking for it.
     * Without ORDER BY, the order in which we emit TIDs does not
     * affect query semantics (executor's recheck filters), but heap
     * order tends to be more cache-friendly.  qsort still gives a
     * stable, deterministic order for tests in either case.
     */
    if (have_orderby)
        qsort(entries, n_entries, sizeof(*entries), knn_entry_cmp);

    st->knn_entries     = entries;
    st->knn_n           = n_entries;
    st->knn_pos         = 0;
    st->knn_compiled    = compiled;
    st->knn_max_cost    = max_cost;
    st->knn_body_attno  = body_attno;
    st->knn_ready       = true;

    MemoryContextSwitchTo(old);

    if (opened_heap)
        table_close(heap, AccessShareLock);
}

bool
pg_tre_amgettuple(IndexScanDesc scan, ScanDirection dir)
{
    TreScanState *st = (TreScanState *) scan->opaque;
    OrderEntry *e;

    /*
     * pg_tre supports only forward scans for KNN ordering.  The result
     * is sorted by distance ascending; backward iteration would be
     * order-by-distance-descending which is meaningless for typical
     * KNN queries.  amcanbackward = false in the AM handler keeps the
     * planner from asking for it; if it does, fail loudly.
     */
    if (dir != ForwardScanDirection)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pg_tre: backward index scan not supported")));

    if (!st->knn_ready)
        knn_build(scan, st);

    if (st->knn_pos >= st->knn_n)
        return false;

    e = &st->knn_entries[st->knn_pos++];
    pg_tre_unpack_tid(e->packed_tid, &scan->xs_heaptid);

    /*
     * Tell the executor to recheck the WHERE clause: our trigram /
     * bloom / positional filters are approximate, and even the post-
     * filter set still contains false positives that survive only
     * because of the conservative-pass rules in tre_compute_candidate_sm.
     */
    scan->xs_recheck = true;

    /*
     * Publish the exact distance we computed.  The executor will use
     * this as the row's ordering value; xs_recheckorderby = false
     * tells it not to recompute (we already did).  ASC NULLS LAST
     * semantics are preserved because we omitted no-match rows from
     * the entries array (they correspond to NULL distances and the
     * user's ORDER BY ... ASC LIMIT N never reaches them anyway).
     *
     * When the planner picked Index Scan without an ORDER BY clause,
     * scan->numberOfOrderBys == 0 and xs_orderbyvals is unallocated;
     * skip the publish entirely.
     */
    if (scan->numberOfOrderBys > 0 && scan->xs_orderbyvals != NULL)
    {
        scan->xs_orderbyvals[0] = Int32GetDatum(e->dist);
        scan->xs_orderbynulls[0] = false;
    }
    scan->xs_recheckorderby = false;

    return true;
}
