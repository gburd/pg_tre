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

#include <string.h>

#include "access/amapi.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "nodes/tidbitmap.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "pg_tre/amapi.h"
#include "pg_tre/page.h"
#include "pg_tre/posting.h"
#include "pg_tre/regex_ast.h"
#include "pg_tre/sparsemap.h"


typedef struct TreScanState
{
    MemoryContext  scan_cxt;   /* per-scan memory context */
    TreParseCtx    parse_ctx;
    TrigramQuery   q;
    bool           query_valid;
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
    scan->opaque = st;
    return scan;
}

void
pg_tre_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                ScanKey orderbys, int norderbys)
{
    TreScanState *st = (TreScanState *) scan->opaque;
    MemoryContext old;
    ScanKey        sk;
    struct TrePatternData *pat;
    char          *pattern_str;
    int            pattern_len;
    int32          max_cost;

    if (keys && scan->numberOfKeys > 0)
        memcpy(scan->keyData, keys,
               scan->numberOfKeys * sizeof(ScanKeyData));

    /* Reset per-scan context. */
    MemoryContextReset(st->scan_cxt);
    st->query_valid = false;

    if (scan->numberOfKeys < 1)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pg_tre: scan requires at least one key")));

    sk = &scan->keyData[0];

    if (sk->sk_strategy != PG_TRE_STRATEGY_APPROX_MATCH)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pg_tre: unsupported scan strategy %d",
                        sk->sk_strategy)));

    old = MemoryContextSwitchTo(st->scan_cxt);

    pat = (struct TrePatternData *) DatumGetPointer(sk->sk_argument);
    pattern_str = tre_pattern_get_text(pat, &pattern_len);
    max_cost = tre_pattern_get_max_cost(pat);

    if (max_cost > 0)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pg_tre: approximate matching (max_cost > 0) "
                        "lands in Phase 5"),
                 errhint("Use tre_pattern(..., 0) or the legacy "
                         "tre_amatch() function for now.")));

    /* Parse regex into AST. */
    if (!tre_parse_regex(&st->parse_ctx, pattern_str, pattern_len))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
                 errmsg("pg_tre: invalid regex pattern: %s",
                        st->parse_ctx.errmsg)));

    /* Extract trigram query. */
    if (!regex_extract_query(&st->parse_ctx, 0, &st->q))
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_tre: trigram extraction failed")));

    st->query_valid = true;
    MemoryContextSwitchTo(old);
}

/*
 * Build the candidate TID sparsemap for one conjunct: OR of its
 * disjuncts.  Returns a palloc'd sparsemap or NULL when the OR is
 * empty (no TIDs match any disjunct in this conjunct -- the whole
 * query has zero matches).
 *
 * Caller is responsible for freeing via free() (it's an owned
 * sparsemap handle) and pfreeing the backing buffer... but since
 * materialized sparsemaps are wrap()-ed over palloc'd buffers that
 * live in CurrentMemoryContext, releasing the context reclaims
 * everything.
 */
static sparsemap_t *
resolve_conjunct(Relation index, const TrigramConjunct *conj,
                 MemoryContext cxt)
{
    sparsemap_t *accum = NULL;
    int i;

    for (i = 0; i < conj->n; i++)
    {
        PgTreUpperRef ref;
        sparsemap_t  *sm;

        if (!pg_tre_upper_lookup(index, conj->alts[i].trigram_hash, &ref))
            continue;               /* trigram absent from index */

        sm = pg_tre_posting_materialize(index, ref.root,
                                        ref.inline_data,
                                        ref.inline_bytes);
        pg_tre_upper_release(&ref);

        if (accum == NULL)
        {
            accum = sm;
        }
        else
        {
            MemoryContext old = MemoryContextSwitchTo(cxt);
            sparsemap_t *merged = sparsemap_union(accum, sm);
            MemoryContextSwitchTo(old);
            free(accum);
            free(sm);
            accum = merged;
        }
    }

    return accum;   /* may be NULL: no posting found for any alt */
}

/*
 * Scanner callback: reserved for a future fast-path using sparsemap_scan
 * (which emits 64-bit vectors).  Phase 3 walks set bits via
 * sparsemap_contains instead; the callback version becomes useful in
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

int64
pg_tre_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
    TreScanState *st = (TreScanState *) scan->opaque;
    MemoryContext old;
    sparsemap_t  *result = NULL;
    int64         ntids = 0;
    int           i;
    uint64        idx, maxidx;

    if (!st->query_valid)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_tre: amgetbitmap called without amrescan")));

    /*
     * Phase 3: if extraction gave up (always_true), we must return
     * every TID for correctness.  But our posting lists only contain
     * trigram-bearing rows, so "every TID" isn't something we can
     * produce cheaply from an inverted index.  The correct behavior
     * is to refuse the scan and let the planner pick seq-scan.
     * Phase 6's cost estimator is supposed to prevent ever reaching
     * this point; in Phase 3 we surface it as a clear error to aid
     * diagnosis in tests.
     */
    if (st->q.always_true)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("pg_tre: regex extraction produced no selective "
                        "trigrams (always_true); index scan cannot answer "
                        "this query"),
                 errhint("Use seq-scan (SET enable_seqscan=on) or a "
                         "pattern with at least one literal trigram.")));

    old = MemoryContextSwitchTo(st->scan_cxt);

    /*
     * Walk conjuncts (AND): for each, compute the OR of its disjuncts;
     * intersect into `result`.  Bail early if any conjunct produces
     * NULL -- that means no rows match the AND-of-ORs query.
     */
    for (i = 0; i < st->q.n; i++)
    {
        sparsemap_t *sm = resolve_conjunct(scan->indexRelation,
                                           &st->q.conjuncts[i],
                                           st->scan_cxt);
        if (sm == NULL)
        {
            /* No posting found for any disjunct in this conjunct --
             * nothing matches. */
            if (result != NULL) free(result);
            result = NULL;
            goto done;
        }

        if (result == NULL)
        {
            result = sm;
        }
        else
        {
            sparsemap_t *merged = sparsemap_intersection(result, sm);
            free(result);
            free(sm);
            result = merged;
            if (result == NULL ||
                (sparsemap_get_size(result) != 0 &&
                 sparsemap_cardinality(result) == 0))
            {
                /* Intersection empty. */
                if (result) free(result);
                result = NULL;
                goto done;
            }
        }
    }

done:
    /* Emit TIDs to the TIDBitmap. */
    if (result != NULL)
    {
        /*
         * Walk the set bits via contains() in [min, max].  This is O(n)
         * where n = range span; for Phase 3 fixtures this is fine.
         * Phase 5 uses the faster sparsemap_scan callback path once
         * we wire the chunk-base index through.
         */
        maxidx = sparsemap_maximum(result);
        idx    = sparsemap_minimum(result);

        if (sparsemap_cardinality(result) > 0)
        {
            while (idx <= maxidx)
            {
                if (sparsemap_contains(result, idx))
                {
                    ItemPointerData tid;
                    pg_tre_unpack_tid(idx, &tid);
                    tbm_add_tuples(tbm, &tid, 1, true /* lossy */);
                    ntids++;
                }
                if (idx == UINT64_MAX) break;
                idx++;
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
