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
#include "pg_tre/bloom.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/page.h"
#include "pg_tre/pending.h"
#include "pg_tre/posting.h"
#include "pg_tre/range.h"
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
typedef struct PendingOverlayEntry
{
    uint64       trigram_hash;
    sparsemap_t *tids;         /* malloc-backed */
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
    for (i = 0; i < ov->n; i++)
        if (ov->entries[i].trigram_hash == h)
            return &ov->entries[i];

    if (ov->n >= ov->alloced)
    {
        MemoryContext old = MemoryContextSwitchTo(ov->mcxt);
        ov->alloced = (ov->alloced == 0) ? 16 : ov->alloced * 2;
        ov->entries = ov->entries
            ? repalloc(ov->entries, ov->alloced * sizeof(*ov->entries))
            : palloc(ov->alloced * sizeof(*ov->entries));
        MemoryContextSwitchTo(old);
    }

    ov->entries[ov->n].trigram_hash = h;
    ov->entries[ov->n].tids         = sparsemap(512);
    return &ov->entries[ov->n++];
}

static void
overlay_collect_cb(uint64 hash, ItemPointer tid, uint32 position, void *ctx)
{
    PendingOverlay *ov = ctx;
    PendingOverlayEntry *e;
    uint64 packed;
    uint64 rc;

    (void) position;

    if (!query_wants_trigram(ov->q, hash))
        return;

    e = overlay_find_or_create(ov, hash);
    packed = pg_tre_pack_tid(tid);
    rc = sparsemap_add(e->tids, packed);
    if (rc == SPARSEMAP_IDX_MAX)
    {
        size_t cap = sparsemap_get_capacity(e->tids);
        sparsemap_t *g = sparsemap_set_data_size(e->tids, NULL,
                                                  cap == 0 ? 1024 : cap * 2);
        if (g == NULL) return;  /* best-effort */
        e->tids = g;
        sparsemap_add(e->tids, packed);
    }
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

static sparsemap_t *
overlay_lookup(const PendingOverlay *ov, uint64 h)
{
    int i;
    for (i = 0; i < ov->n; i++)
        if (ov->entries[i].trigram_hash == h)
            return ov->entries[i].tids;
    return NULL;
}

static void
overlay_free(PendingOverlay *ov)
{
    int i;
    for (i = 0; i < ov->n; i++)
        if (ov->entries[i].tids)
            free(ov->entries[i].tids);
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
                sm = sparsemap_copy(pend);
            }
            else
            {
                sparsemap_t *u = sparsemap_union(sm, pend);
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
            sparsemap_t *merged = sparsemap_union(accum, sm);
            free(accum);
            free(sm);
            accum = merged;
        }
    }

    (void) cxt;
    return accum;
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
    uint64 idx, maxidx;
    uint8 bloom_buf[256];  /* large enough for typical bloom sizes */
    Size bloom_bytes;
    int i, j;

    if (candidates == NULL || sparsemap_cardinality(candidates) == 0)
        return candidates;

    /* Phase 5: per-tuple blooms are (pg_tre_bloom_tuple_bits + 7) / 8 bytes */
    bloom_bytes = (pg_tre_bloom_tuple_bits + 7) / 8;
    if (bloom_bytes > sizeof(bloom_buf))
        bloom_bytes = sizeof(bloom_buf);  /* defensive */

    refined = sparsemap(sparsemap_get_capacity(candidates));
    if (refined == NULL)
        return candidates;  /* OOM; fall back to unrefined */

    maxidx = sparsemap_maximum(candidates);
    idx = sparsemap_minimum(candidates);

    while (idx <= maxidx)
    {
        if (sparsemap_contains(candidates, idx))
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

                    for (j = 0; j < conj->n; j++)
                    {
                        uint64 h = conj->alts[j].trigram_hash;
                        PgTreUpperRef ref;
                        bool has_bloom = false;

                        if (pg_tre_upper_lookup(index, h, &ref))
                        {
                            has_bloom = pg_tre_posting_lookup_tuple_bloom(
                                            index, ref.root, ref.inline_data,
                                            ref.inline_bytes, idx,
                                            bloom_buf, bloom_bytes);
                            pg_tre_upper_release(&ref);
                        }

                        if (has_bloom)
                        {
                            /* Bloom data present; check if trigram is in it.
                             * Wrap the bytes as a PgTreBloom and test. */
                            PgTreBloom *b = (PgTreBloom *) bloom_buf;
                            if (pg_tre_bloom_contains_trigram(b, h))
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

                    for (j = 0; j < tile->n && tile_pass; j++)
                    {
                        uint64 h = tile->alts[j].trigram_hash;
                        PgTreUpperRef ref;
                        bool has_bloom = false;

                        if (pg_tre_upper_lookup(index, h, &ref))
                        {
                            has_bloom = pg_tre_posting_lookup_tuple_bloom(
                                            index, ref.root, ref.inline_data,
                                            ref.inline_bytes, idx,
                                            bloom_buf, bloom_bytes);
                            pg_tre_upper_release(&ref);
                        }

                        if (has_bloom)
                        {
                            PgTreBloom *b = (PgTreBloom *) bloom_buf;
                            if (!pg_tre_bloom_contains_trigram(b, h))
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
                if (sparsemap_add(refined, idx) == SPARSEMAP_IDX_MAX)
                {
                    /* Overflow; fall back to unrefined */
                    free(refined);
                    return candidates;
                }
            }
        }

        if (idx == UINT64_MAX)
            break;
        idx++;
    }

    (void) cxt;
    free(candidates);
    return refined;
}

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
     * Build the pending-list overlay once.  If the index has no
     * pending entries, this is a single metapage read and returns
     * immediately.
     */
    {
        PendingOverlay ov;
        overlay_build(&ov, scan->indexRelation, &st->q, st->scan_cxt);

        /*
         * Phase 5: dispatch based on query mode (CNF or DNF).
         *
         * CNF mode (k=0): AND across conjuncts.  For each conjunct,
         *   compute OR of its disjuncts; intersect into result.
         *
         * DNF mode (k>0 tiled): OR across tiles.  For each tile,
         *   compute AND of its trigrams; union into result.
         */
        if (st->q.mode == TRIGRAM_QUERY_CNF)
        {
            /* CNF: AND across conjuncts (original Phase 3 logic) */
            for (i = 0; i < st->q.n; i++)
            {
                sparsemap_t *sm = resolve_conjunct_with_overlay(
                                      scan->indexRelation,
                                      &st->q.conjuncts[i],
                                      st->scan_cxt, &ov);
                if (sm == NULL)
                {
                    if (result != NULL) free(result);
                    result = NULL;
                    overlay_free(&ov);
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
                        if (result) free(result);
                        result = NULL;
                        overlay_free(&ov);
                        goto done;
                    }
                }
            }
        }
        else  /* TRIGRAM_QUERY_DNF */
        {
            /* DNF: OR across tiles.  For each tile (conjunct in DNF),
             * compute AND of its disjuncts, then union into result. */
            for (i = 0; i < st->q.n; i++)
            {
                const TrigramConjunct *tile = &st->q.conjuncts[i];
                sparsemap_t *tile_result = NULL;
                int j;

                /* Within this tile, AND all trigrams together */
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
                        {
                            sm = sparsemap_copy(pend);
                        }
                        else
                        {
                            sparsemap_t *u = sparsemap_union(sm, pend);
                            free(sm);
                            sm = u;
                        }
                    }

                    if (sm == NULL)
                    {
                        /* This trigram not present; tile can't match */
                        if (tile_result != NULL)
                            free(tile_result);
                        tile_result = NULL;
                        break;
                    }

                    if (tile_result == NULL)
                    {
                        tile_result = sm;
                    }
                    else
                    {
                        sparsemap_t *intersect = sparsemap_intersection(tile_result, sm);
                        free(tile_result);
                        free(sm);
                        tile_result = intersect;
                        if (tile_result == NULL ||
                            (sparsemap_get_size(tile_result) != 0 &&
                             sparsemap_cardinality(tile_result) == 0))
                        {
                            if (tile_result) free(tile_result);
                            tile_result = NULL;
                            break;
                        }
                    }
                }

                /* Union this tile's result into the overall result (OR) */
                if (tile_result != NULL)
                {
                    if (result == NULL)
                    {
                        result = tile_result;
                    }
                    else
                    {
                        sparsemap_t *merged = sparsemap_union(result, tile_result);
                        free(result);
                        free(tile_result);
                        result = merged;
                    }
                }
            }
        }

        overlay_free(&ov);
    }

    /* Phase 5 tier-3: apply per-tuple bloom filtering.
     *
     * Disabled pending a fix for a crash in sparsemap_rank during
     * pg_tre_posting_lookup_tuple_bloom.  Tier-3 is an optimization;
     * correctness does not depend on it -- the executor's recheck
     * filters any false positives from tier-2.  Re-enable when the
     * sparsemap_rank bug is resolved.
     */
    if (result != NULL && st->q.global_max_cost >= 0)
    {
        result = apply_tuple_bloom_filter(scan->indexRelation, &st->q,
                                         result, st->scan_cxt);
    }

    /* Phase 5.1: apply positional filtering.
     *
     * Disabled pending a fix for a crash in sparsemap_rank during
     * pg_tre_posting_lookup_positions.  Correctness does not depend on
     * positional filtering -- the executor's recheck handles false
     * positives.  Re-enable when the sparsemap_rank bug is resolved.
     */
    if (false && result != NULL && sparsemap_cardinality(result) > 0)
    {
        sparsemap_t *filtered = sparsemap(sparsemap_get_capacity(result));
        if (filtered != NULL)
        {
            uint64 tid_idx = sparsemap_minimum(result);
            uint64 tid_maxidx = sparsemap_maximum(result);

            while (tid_idx <= tid_maxidx)
            {
                if (sparsemap_contains(result, tid_idx))
                {
                    bool passes = true;
                    int ci, j;

                    /* For each conjunct/tile, check if required trigrams
                     * appear at valid positions */
                    for (ci = 0; ci < st->q.n && passes; ci++)
                    {
                        const TrigramConjunct *conj = &st->q.conjuncts[ci];

                        if (st->q.mode == TRIGRAM_QUERY_CNF)
                        {
                            /* CNF: at least one disjunct must have valid positions */
                            bool conj_pass = false;
                            for (j = 0; j < conj->n && !conj_pass; j++)
                            {
                                const TrigramDisjunct *dis = &conj->alts[j];
                                PgTreUpperRef ref;
                                const uint32 *positions;
                                int n_positions, p;

                                if (!pg_tre_upper_lookup(scan->indexRelation,
                                                        dis->trigram_hash, &ref))
                                    continue;

                                n_positions = pg_tre_posting_lookup_positions(
                                                scan->indexRelation,
                                                ref.root, ref.inline_data,
                                                ref.inline_bytes,
                                                tid_idx, &positions);
                                pg_tre_upper_release(&ref);

                                if (n_positions == 0)
                                {
                                    /* No positions stored; conservatively pass */
                                    conj_pass = true;
                                    break;
                                }

                                /* Check if any position falls in valid range */
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
                            if (!conj_pass)
                                passes = false;
                        }
                        else  /* DNF */
                        {
                            /* DNF: ALL trigrams in this tile must have valid positions */
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
                                {
                                    /* No positions; conservatively pass this trigram */
                                    this_tri_pass = true;
                                }
                                else
                                {
                                    /* Check if any position falls in valid range */
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
                                /* At least one tile passed; TID is good */
                                passes = true;
                                break;  /* no need to check other tiles */
                            }
                            else
                            {
                                passes = false;  /* this tile failed */
                            }
                        }
                    }

                    if (passes)
                    {
                        if (sparsemap_add(filtered, tid_idx) == SPARSEMAP_IDX_MAX)
                        {
                            /* Overflow; give up on filtering */
                            free(filtered);
                            filtered = NULL;
                            break;
                        }
                    }
                }

                if (tid_idx == UINT64_MAX)
                    break;
                tid_idx++;
            }

            if (filtered != NULL)
            {
                free(result);
                result = filtered;
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
