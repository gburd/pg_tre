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
 * Phase 5 extends amgetbitmap with the tier-1 range bloom and k>0 via
 * Navarro tiling.  (The per-tuple bloom / positional tier-3 filters were
 * removed in 3.0.0; the executor recheck is authoritative.)
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
#include "miscadmin.h"
#include "nodes/tidbitmap.h"
#include "storage/bufmgr.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "pg_tre/like_translate.h"
#include "pg_tre/amapi.h"
#include "pg_tre/bloom.h"
#include "pg_tre/pattern_cache.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/page.h"
#include "pg_tre/pending.h"
#include "pg_tre/posting.h"
#include "pg_tre/range.h"
#include "pg_tre/regex_ast.h"
#include "pg_tre/run_catalog.h"
#include "pg_tre/sparsemap.h"
#include "pg_tre/tre_match.h"

/*
 * Pinning entrypoints for the compiled-pattern cache.  Declared here
 * because pattern_cache.h is shared and owned elsewhere; the pin/unpin
 * API is consumed only by this translation unit (the KNN scan loop).
 * tre_cache_lookup_pinned() returns a compiled handle whose cache slot
 * is pinned against LRU eviction until tre_cache_release() drops the
 * pin -- without this, a long re-entrant scan that compiles >= 32 other
 * patterns could evict and free the handle still in use (use-after-free).
 */
extern void *tre_cache_lookup_pinned(const char *pattern, int pattern_len);
extern void tre_cache_release(void *compiled);


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
        switch (sk->sk_strategy)
        {
            case PG_TRE_STRATEGY_APPROX_MATCH:
            case PG_TRE_STRATEGY_LIKE:
            case PG_TRE_STRATEGY_ILIKE:
            case PG_TRE_STRATEGY_REGEX:
            case PG_TRE_STRATEGY_IREGEX:
            case PG_TRE_STRATEGY_EQUAL:
                break;
            default:
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("pg_tre: unsupported scan strategy %d",
                                sk->sk_strategy)));
        }
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

    /*
     * Acquire the pattern string + edit-distance budget.
     *
     * Strategy 1 (%~~) and 2 (<@>) carry a tre_pattern RHS with an
     * explicit max_cost.  The A1 parity strategies (LIKE/ILIKE/
     * regex/iregex/=) carry a plain text RHS and always run at k=0
     * (exact); they are lowered into the same regex/trigram engine,
     * and the executor rechecks with the built-in operator so the
     * index is a lossy candidate filter only.
     */
    if (sk->sk_strategy == PG_TRE_STRATEGY_APPROX_MATCH ||
        sk->sk_strategy == PG_TRE_STRATEGY_DISTANCE)
    {
        pat = (struct TrePatternData *) DatumGetPointer(sk->sk_argument);
        pattern_str = tre_pattern_get_text(pat, &pattern_len);
        max_cost = tre_pattern_get_max_cost(pat);
    }
    else
    {
        /* text RHS: detoast, translate to regex by strategy, k=0. */
        text   *rhs = DatumGetTextPP(sk->sk_argument);
        char   *raw = VARDATA_ANY(rhs);
        int     rawlen = VARSIZE_ANY_EXHDR(rhs);

        max_cost = 0;
        switch (sk->sk_strategy)
        {
            case PG_TRE_STRATEGY_LIKE:
            case PG_TRE_STRATEGY_ILIKE:
                pattern_str = pg_tre_like_to_regex(raw, rawlen, '\\');
                pattern_len = (int) strlen(pattern_str);
                break;
            case PG_TRE_STRATEGY_REGEX:
            case PG_TRE_STRATEGY_IREGEX:
                /* already a regex; copy into scan ctx (NUL-safe len). */
                pattern_str = (char *) palloc(rawlen + 1);
                memcpy(pattern_str, raw, rawlen);
                pattern_str[rawlen] = '\0';
                pattern_len = rawlen;
                break;
            case PG_TRE_STRATEGY_EQUAL:
                pattern_str = pg_tre_literal_to_regex(raw, rawlen);
                pattern_len = (int) strlen(pattern_str);
                break;
            default:
                pattern_str = NULL;
                pattern_len = 0;
        }
    }

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

    /*
     * Case-insensitive operators (ILIKE / ~*) cannot be safely
     * trigram-accelerated: the index stores case-sensitive trigrams
     * of the original text, so a differently-cased query would miss
     * real matches (e.g. ILIKE '%NEEDLE%' against indexed 'needle').
     * Force the always-true path so amgetbitmap emits a fully-lossy
     * bitmap and the executor's recheck (texticlike / texticregexeq)
     * filters every row -- correct, if no faster than a seq scan.
     * A case-folded index is future work (would let these accelerate).
     */
    if (sk->sk_strategy == PG_TRE_STRATEGY_ILIKE ||
        sk->sk_strategy == PG_TRE_STRATEGY_IREGEX)
        st->q.always_true = true;

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
    sm_t *tids;         /* lazily built on first overlay_lookup */
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

static sm_t *
overlay_lookup(const PendingOverlay *ov, uint64 h)
{
    int i, k;
    PendingOverlayEntry *e;
    sm_t *sm;

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
 * Phase B1.5: per-scan crack cache (lazy per-trigram materialization).
 *
 * A multi-run scan resolves each query trigram by iterating every run
 * and unioning the per-run postings.  When the same trigram_hash recurs
 * across conjuncts/disjuncts of a query, the un-cracked path repeats
 * that whole O(runs) iteration on every touch.  The crack cache
 * materializes a trigram's union-across-runs sparsemap ON FIRST TOUCH
 * and reuses it for every later touch in the same scan (and for the
 * co-occurring trigrams of the rows touched, since they share the
 * cache) -- this is the "cracking payoff" realized as a pure in-memory,
 * read-only optimization.
 *
 * Safety: the cache writes nothing to disk, takes no locks, and is torn
 * down with the scan.  Results are byte-identical to the un-cracked
 * path (it caches the exact same union), so the executor recheck
 * contract is untouched.  It is therefore correct on a hot standby /
 * read-only transaction with no guard.  It is gated behind the
 * default-off pg_tre.crack_on_read GUC only because the durable
 * write-back (which WOULD need standby/crash guards) is deferred; see
 * doc/specs/phaseB1-run-catalog.md B1.5.
 *
 * The cached sparsemaps are malloc'd (sm_t lives in malloc, like every
 * other sm in this file); the cache owns them and frees them in
 * crack_cache_destroy.  The struct itself is palloc'd in scan_cxt.
 * Lookups return a COPY so the caller's union/free bookkeeping is
 * unchanged.  ponytail: linear-probe open addressing -- a scan touches
 * O(query trigrams) distinct hashes (tens), not millions; a tree/hash
 * map would be more code for no measurable win at this size.
 */
typedef struct CrackCacheEntry
{
    uint64  hash;
    bool    used;       /* slot occupied */
    bool    present;    /* trigram resolved to a non-NULL union */
    sm_t   *runs_sm;    /* malloc'd union across runs, or NULL */
} CrackCacheEntry;

typedef struct CrackCache
{
    int               cap;       /* power of two; 0 means disabled */
    int               n;         /* occupied slots */
    CrackCacheEntry  *slots;     /* palloc0'd in mcxt, cap entries */
} CrackCache;

static void
crack_cache_init(CrackCache *cc, const TrigramQuery *q, MemoryContext mcxt)
{
    int total = 0;
    int cap = 8;
    int i;

    cc->cap = 0;
    cc->n = 0;
    cc->slots = NULL;

    if (!pg_tre_crack_on_read || q == NULL || q->n <= 0)
        return;

    for (i = 0; i < q->n; i++)
        total += q->conjuncts[i].n;
    if (total <= 0)
        return;

    /* Size to >= 2x the max distinct hashes so the table stays sparse. */
    while (cap < total * 2)
        cap <<= 1;

    cc->slots = (CrackCacheEntry *)
        MemoryContextAllocZero(mcxt, sizeof(CrackCacheEntry) * cap);
    cc->cap = cap;
}

static void
crack_cache_destroy(CrackCache *cc)
{
    int i;

    if (cc->slots == NULL)
        return;
    for (i = 0; i < cc->cap; i++)
        if (cc->slots[i].used && cc->slots[i].runs_sm != NULL)
            free(cc->slots[i].runs_sm);
    /* slots themselves are palloc'd in the scan context. */
    cc->slots = NULL;
    cc->cap = 0;
    cc->n = 0;
}

/* Find the slot for hash (occupied or the empty slot to fill). */
static inline CrackCacheEntry *
crack_cache_slot(CrackCache *cc, uint64 hash)
{
    uint32 mask = (uint32) (cc->cap - 1);
    uint32 i = (uint32) hash & mask;

    for (;;)
    {
        CrackCacheEntry *e = &cc->slots[i];
        if (!e->used || e->hash == hash)
            return e;
        i = (i + 1) & mask;
    }
}

/*
 * Resolve one trigram's union across ALL live runs.
 *
 * Returns a fresh malloc'd sparsemap the caller owns (or NULL if no
 * run holds the trigram).  When the crack cache is enabled it
 * materializes the union once per trigram per scan and returns a copy
 * on subsequent touches; otherwise it does the run iteration every
 * time (the pre-B1.5 behavior).
 */
static sm_t *
resolve_trigram_runs(Relation index, uint64 h, CrackCache *cc)
{
    CrackCacheEntry *slot = NULL;
    PgTreRunIter    *it;
    PgTreRun         run;
    sm_t            *sm = NULL;

    if (cc != NULL && cc->slots != NULL)
    {
        slot = crack_cache_slot(cc, h);
        if (slot->used)
        {
            /* Cache hit. */
            if (!slot->present || slot->runs_sm == NULL)
                return NULL;            /* known: no run holds this trigram */
            {
                sm_t *hit = sm_copy(slot->runs_sm);
                if (hit != NULL)
                    return hit;
                /* Copy OOM: fall through and recompute from the runs
                 * rather than returning NULL and dropping candidates. */
                slot = NULL;
            }
        }
    }

    it = pg_tre_run_catalog_open(index);
    while (pg_tre_run_catalog_next(it, &run))
    {
        PgTreUpperRef ref;
        sm_t         *run_sm;

        /* Run-skip range filter (the Surf analogue). */
        if (h < run.min_trigram_hash || h > run.max_trigram_hash)
            continue;

        if (!pg_tre_upper_lookup_root(index, run.root_upper, h, &ref))
            continue;

        run_sm = pg_tre_posting_materialize(index, ref.root,
                                            ref.inline_data,
                                            ref.inline_bytes);
        pg_tre_upper_release(&ref);
        if (run_sm == NULL)
            continue;

        if (sm == NULL)
        {
            sm = run_sm;
        }
        else
        {
            sm_t *u = sm_union(sm, run_sm);
            free(sm);
            free(run_sm);
            sm = u;
        }
    }
    pg_tre_run_catalog_close(it);

    if (slot != NULL)
    {
        /*
         * Populate the cache; store a copy so we can hand the caller
         * the original (avoids an extra copy on the first touch).  If
         * the copy fails (OOM) or there were no postings, leave the
         * slot occupied with runs_sm = NULL and present reflecting
         * reality -- a later hit recomputes only when present is true
         * but runs_sm is NULL would drop candidates, so we must NOT
         * mark present=true without a valid copy.  When the copy fails
         * for a non-empty union, leave the slot UNused so the next
         * touch recomputes correctly (correctness over the cache win).
         */
        if (sm == NULL)
        {
            slot->used = true;
            slot->hash = h;
            slot->present = false;
            slot->runs_sm = NULL;
            cc->n++;
        }
        else
        {
            sm_t *cached = sm_copy(sm);
            if (cached != NULL)
            {
                slot->used = true;
                slot->hash = h;
                slot->present = true;
                slot->runs_sm = cached;
                cc->n++;
            }
            /* else: leave slot unused; next touch recomputes. */
        }
    }
    return sm;
}

/*
 * Build the candidate TID sparsemap for one conjunct: OR of its
 * disjuncts, including the pending-list overlay for each trigram.
 */
static sm_t *
resolve_conjunct_with_overlay(Relation index, const TrigramConjunct *conj,
                              MemoryContext cxt, const PendingOverlay *ov,
                              CrackCache *cc)
{
    sm_t *accum = NULL;
    int i;

    for (i = 0; i < conj->n; i++)
    {
        uint64 h = conj->alts[i].trigram_hash;
        sm_t  *sm;
        sm_t  *pend;

        /*
         * Phase B1.2: resolve the trigram across ALL runs and union
         * the per-run postings (newest-run-wins is moot for B1.2
         * inserts-only: a TID present in any run is a candidate; the
         * executor rechecks).  For the common single-run index the
         * iterator yields exactly one run rooted at the index roots,
         * so this is identical to the pre-B1.2 single lookup.
         *
         * Phase B1.5: resolve_trigram_runs memoizes this union per
         * trigram per scan (crack cache) when pg_tre.crack_on_read is
         * on; with the GUC off it iterates the runs every time, exactly
         * as before.
         */
        sm = resolve_trigram_runs(index, h, cc);

        pend = overlay_lookup(ov, h);
        if (pend != NULL)
        {
            if (sm == NULL)
            {
                sm = sm_copy(pend);
            }
            else
            {
                sm_t *u = sm_union(sm, pend);
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
            sm_t *merged = sm_union(accum, sm);
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
 * sm_contains instead.
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
 * Compute the candidate-TID sparsemap for the current scan.
 *
 * Caller must have st->scan_cxt as the current memory context.  The
 * returned sparsemap is malloc'd (not palloc'd); caller frees it via
 * free().  Returns NULL when the result is empty OR when the query is
 * always_true (in the latter case *out_always_true is set).
 */
static sm_t *
tre_compute_candidate_sm(IndexScanDesc scan, TreScanState *st,
                         bool *out_always_true)
{
    volatile sm_t *result = NULL;
    /*
     * Maps owned transiently during the build.  Tracked in volatile
     * locals so the PG_CATCH below can free them if any ereport-capable
     * call (pg_tre_posting_materialize, sm_intersection/sm_union/sm_copy,
     * sm_add) longjmps out mid-build -- otherwise these malloc-backed
     * maps leak (H3).
     */
    volatile sm_t *inflight = NULL;   /* transient sm being merged */
    PendingOverlay ov;
    volatile bool  overlay_built = false;
    CrackCache     crack = {0};
    volatile bool  crack_built = false;
    volatile bool  short_circuit = false;   /* CNF proved empty: result == NULL */
    int           i;

    *out_always_true = false;

    if (st->q.always_true)
    {
        *out_always_true = true;
        return NULL;
    }

    PG_TRY();
    {
    /*
     * Build the pending-list overlay once.  If the index has no
     * pending entries, this is a single metapage read and returns
     * immediately.
     */
    {
        overlay_build(&ov, scan->indexRelation, &st->q, st->scan_cxt);
        overlay_built = true;

        /*
         * Phase B1.5: per-scan crack cache.  No-op (cap 0) unless
         * pg_tre.crack_on_read is on.  Memoizes each trigram's
         * union-across-runs so a trigram touched K times in this scan
         * iterates the runs once, not K times.
         */
        crack_cache_init(&crack, &st->q, st->scan_cxt);
        crack_built = true;

        if (st->q.mode == TRIGRAM_QUERY_CNF)
        {
            /* CNF: AND across conjuncts. */
            for (i = 0; i < st->q.n; i++)
            {
                sm_t *sm = resolve_conjunct_with_overlay(
                                      scan->indexRelation,
                                      &st->q.conjuncts[i],
                                      st->scan_cxt, &ov, &crack);
                if (sm == NULL)
                {
                    if (result != NULL) { free((sm_t *) result); result = NULL; }
                    overlay_free(&ov);
                    overlay_built = false;
                    short_circuit = true;
                    break;
                }

                if (result == NULL)
                {
                    result = sm;
                }
                else
                {
                    sm_t *merged;
                    inflight = sm;
                    merged = sm_intersection((sm_t *) result, sm);
                    free((sm_t *) result);
                    free(sm);
                    inflight = NULL;
                    result = merged;
                    if (result == NULL ||
                        (sm_get_size((sm_t *) result) != 0 &&
                         sm_cardinality((sm_t *) result) == 0))
                    {
                        if (result) { free((sm_t *) result); result = NULL; }
                        overlay_free(&ov);
                        overlay_built = false;
                        short_circuit = true;
                        break;
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
                    sm_t *sm = NULL;
                    sm_t *pend;

                    if (pg_tre_upper_lookup(scan->indexRelation,
                                           tile->alts[j].trigram_hash, &ref))
                    {
                        sm = pg_tre_posting_materialize(scan->indexRelation,
                                                       ref.root,
                                                       ref.inline_data,
                                                       ref.inline_bytes);
                        pg_tre_upper_release(&ref);
                    }
                    inflight = sm;

                    pend = overlay_lookup(&ov, tile->alts[j].trigram_hash);
                    if (pend != NULL)
                    {
                        if (sm == NULL)
                            sm = sm_copy(pend);
                        else
                        {
                            sm_t *u = sm_union(sm, pend);
                            free(sm);
                            sm = u;
                        }
                        inflight = sm;
                    }

                    if (sm == NULL)
                    {
                        inflight = NULL;
                        continue;
                    }

                    if (result == NULL)
                    {
                        result = sm;
                        inflight = NULL;
                    }
                    else
                    {
                        sm_t *merged = sm_union((sm_t *) result, sm);
                        free((sm_t *) result);
                        free(sm);
                        inflight = NULL;
                        result = merged;
                    }
                }
            }
        }

        if (!short_circuit)
        {
            overlay_free(&ov);
            overlay_built = false;
        }
    }


    crack_cache_destroy(&crack);
    crack_built = false;
    }
    PG_CATCH();
    {
        /*
         * H3: a longjmp out of any ereport-capable call above skips the
         * manual frees.  Release every malloc-backed map we still own,
         * plus the overlay, then re-throw.
         */
        if (inflight != NULL)
            free((sm_t *) inflight);
        if (result != NULL)
            free((sm_t *) result);
        if (overlay_built)
            overlay_free(&ov);
        if (crack_built)
            crack_cache_destroy(&crack);
        PG_RE_THROW();
    }
    PG_END_TRY();

    return (sm_t *) result;
}

int64
pg_tre_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
    TreScanState *st = (TreScanState *) scan->opaque;
    MemoryContext old;
    sm_t  *result;
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
        uint64 card = sm_cardinality(result);

        if (card > 0)
        {
            /*
             * Emit all candidate TIDs in a single tbm_add_tuples() call
             * rather than one per TID.  The 'recheck' flag is true
             * because the index's trigram filters and the range-bloom
             * pre-filter are approximate: the executor must recheck the WHERE
             * clause per row.  (This is recheck, NOT lossy block-level
             * matching - the bitmap stays exact at the TID level until
             * it overflows work_mem.)
             */
            ItemPointer tids = (ItemPointer)
                palloc(sizeof(ItemPointerData) * card);
            uint64 i = SM_IDX_MAX;
            int n = 0;
            sm_cursor_t scur = SM_CURSOR_INIT;

            while ((i = sm_next_member(result, i, &scur)) != SM_IDX_MAX)
                pg_tre_unpack_tid(i, &tids[n++]);

            tbm_add_tuples(tbm, tids, n, true /* recheck */);
            ntids += n;
            pfree(tids);
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
 *                              (trigram + range bloom), so the
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
    volatile sm_t *result = NULL;
    bool          always_true = false;
    Relation      heap = scan->heapRelation;
    bool          opened_heap = false;
    AttrNumber    body_attno;
    void * volatile compiled = NULL;
    int32         max_cost = 0;
    struct TrePatternData *pat;
    char         *pat_text;
    int           pat_len;
    int           cap = 64;
    volatile int  n_entries = 0;
    OrderEntry   *volatile entries;
    bool          have_orderby = (scan->numberOfOrderBys >= 1 &&
                                  scan->orderByData != NULL);
    volatile bool deadline_armed = false;

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
     *
     * Pin the compiled pattern: the handle is held across the long
     * heap_getnext / sm_next_member loop below, which can re-enter the
     * planner/executor (toast fetches, etc.) and compile other regexes,
     * evicting an unpinned entry out from under us (use-after-free).
     * The pin is dropped in the PG_FINALLY block.
     */
    if (have_orderby)
    {
        pat = (struct TrePatternData *)
              PG_DETOAST_DATUM(scan->orderByData[0].sk_argument);
        pat_text = tre_pattern_get_text(pat, &pat_len);
        max_cost = tre_pattern_get_max_cost(pat);
        compiled = tre_cache_lookup_pinned(pat_text, pat_len);
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

    /*
     * Consolidated cleanup region (H2 + H3 + C1): the match-timeout
     * deadline, the pinned compiled pattern, and the malloc-backed
     * candidate sparsemap must all be released on both normal and
     * error (longjmp) exit.  A corrupt-page ERROR or OOM inside any of
     * the ereport-capable calls below (heap_fetch, PG_DETOAST_DATUM,
     * tre_do_match, sm_*) would otherwise skip the manual frees and
     * leak / leave the cache entry pinned forever.
     */
    PG_TRY();
    {
        result = tre_compute_candidate_sm(scan, st, &always_true);

        /*
         * Arm the per-match deadline once for the whole loop.  Only
         * meaningful when we actually run tre_do_match (have_orderby);
         * 0 => use the GUC-configured timeout.
         */
        if (have_orderby)
        {
            pg_tre_arm_match_deadline(0);
            deadline_armed = true;
        }

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

                CHECK_FOR_INTERRUPTS();

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
                    pg_tre_check_match_timeout(&r);
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
        else if (result != NULL && sm_cardinality((sm_t *) result) > 0)
        {
            uint64 idx = SM_IDX_MAX;
            sm_cursor_t scur = SM_CURSOR_INIT;

            while ((idx = sm_next_member((sm_t *) result, idx, &scur)) != SM_IDX_MAX)
            {
                ItemPointerData tid;
                int32           dist = 0;

                CHECK_FOR_INTERRUPTS();

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

                    pg_tre_check_match_timeout(&r);
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

        /*
         * Normal-path free of the malloc-backed candidate map.  Clear
         * the pointer so the PG_FINALLY block does not double-free it.
         */
        if (result != NULL)
        {
            free((sm_t *) result);
            result = NULL;
        }
    }
    PG_FINALLY();
    {
        if (deadline_armed)
            pg_tre_disarm_match_deadline();
        if (result != NULL)
            free((sm_t *) result);
        if (compiled != NULL)
            tre_cache_release(compiled);
    }
    PG_END_TRY();

    /*
     * Sorting is only meaningful when ORDER BY is asking for it.
     * Without ORDER BY, the order in which we emit TIDs does not
     * affect query semantics (executor's recheck filters), but heap
     * order tends to be more cache-friendly.  qsort still gives a
     * stable, deterministic order for tests in either case.
     */
    if (have_orderby)
        qsort((void *) entries, n_entries, sizeof(*entries), knn_entry_cmp);

    st->knn_entries     = (OrderEntry *) entries;
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
     * Tell the executor to recheck the WHERE clause: our trigram
     * filters and the range-bloom pre-filter are approximate, and even
     * the post-
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
