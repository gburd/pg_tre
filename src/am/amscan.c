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
#include "access/table.h"
#include "nodes/tidbitmap.h"
#include "storage/bufmgr.h"
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
 * construction (see ambuild.c::record_tid_trigram and
 * materialize_tid_bloom), and the only
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
 * Format v4: each on-disk slot carries a (width_code, k) header.  The
 * scan reads those out via pg_tre_posting_lookup_tuple_bloom and uses
 * pg_tre_bloom_contains_trigram_raw against the returned bit array.
 * Slots with k == 0 are the always-pass sentinel and contribute no
 * pruning power.
 */
static sparsemap_t *
apply_tuple_bloom_filter(Relation index, const TrigramQuery *q,
                        sparsemap_t *candidates, MemoryContext cxt)
{
    sparsemap_t *refined;
    uint64 idx;
    /*
     * Bloom scratch buffer.  Format-v4 widths max out at 1024 bits =
     * 128 bytes; this buffer is sized to hold the largest possible
     * bloom regardless of the GUC cap.
     */
    uint8 bloom_bits[(PG_TRE_MAX_BLOOM_BITS + 7) / 8];
    int i, j;

    if (candidates == NULL || sm_cardinality(candidates) == 0)
        return candidates;

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

                    for (j = 0; j < conj->n; j++)
                    {
                        uint64 h = conj->alts[j].trigram_hash;
                        PgTreUpperRef ref;
                        bool has_bloom = false;
                        uint16 m_bits = 0;
                        uint8  k = 0;

                        if (pg_tre_upper_lookup(index, h, &ref))
                        {
                            has_bloom = pg_tre_posting_lookup_tuple_bloom(
                                            index, ref.root, ref.inline_data,
                                            ref.inline_bytes, idx,
                                            bloom_bits, sizeof(bloom_bits),
                                            &m_bits, &k);
                            pg_tre_upper_release(&ref);
                        }

                        if (has_bloom)
                        {
                            /* k == 0 (always-pass sentinel) yields true
                             * unconditionally; otherwise probe the bloom. */
                            if (pg_tre_bloom_contains_trigram_raw(
                                    bloom_bits, m_bits, k, h))
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
                        uint16 m_bits = 0;
                        uint8  k = 0;

                        if (pg_tre_upper_lookup(index, h, &ref))
                        {
                            has_bloom = pg_tre_posting_lookup_tuple_bloom(
                                            index, ref.root, ref.inline_data,
                                            ref.inline_bytes, idx,
                                            bloom_bits, sizeof(bloom_bits),
                                            &m_bits, &k);
                            pg_tre_upper_release(&ref);
                        }

                        if (has_bloom)
                        {
                            if (!pg_tre_bloom_contains_trigram_raw(
                                    bloom_bits, m_bits, k, h))
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

int64
pg_tre_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
    TreScanState *st = (TreScanState *) scan->opaque;
    MemoryContext old;
    sparsemap_t  *result = NULL;
    int64         ntids = 0;
    int           i;

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
     * correctness is preserved.  This path is reached when:
     *   - pattern has no literal run >= 3 chars to anchor trigrams
     *   - global k is so large that Navarro tiling has no spine
     *   - per-tile uleven expansion exceeds max_extraction_fanout
     * The cost estimator already reports disable_cost so the planner
     * normally picks a seq-scan; this fallback only fires when
     * enable_seqscan=off has forced index use.
     */
    if (st->q.always_true)
    {
        Relation   heap = scan->heapRelation;
        BlockNumber  nblocks;
        BlockNumber  blk;
        int64        emitted = 0;

        /*
         * scan->heapRelation is set by the executor for index-only and
         * bitmap scans before amgetbitmap is called.  If for any reason
         * it is not available, fall back to the index relation's heap
         * via the catalog.
         */
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
                    sparsemap_t *merged = sm_intersection(result, sm);
                    free(result);
                    free(sm);
                    result = merged;
                    if (result == NULL ||
                        (sm_get_size(result) != 0 &&
                         sm_cardinality(result) == 0))
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
            /*
             * DNF semantics from Navarro tiling: at least one tile
             * must have at least one of its alternative trigrams
             * present in the row.  Within a tile the alternatives
             * are OR (the trigram at this spine position OR any of
             * its k=1 / k=2 universal-Levenshtein neighbours).  Across
             * tiles is also OR (pigeonhole: with k edits and k+1
             * tiles, at least one tile is untouched).  Both layers
             * are OR, so the candidate set is the union of all
             * posting lists referenced by any disjunct of any tile.
             */
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
                        continue;  /* trigram not present in any row */

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

    /* Phase 5 tier-3: apply per-tuple bloom filtering.
     *
     * The bloom payload lives in the posting leaf at an offset keyed by
     * sm_rank(map, 0, tid, true) - 1.  When a posting tree splits across
     * a Lehman-Yao right-link chain, that rank is currently computed only
     * within the leaf containing the TID, not across all preceding leaves
     * in the chain.  The result is that for any TID landing past the first
     * leaf, the rank used to index into the payload is off by the
     * accumulated cardinality of the earlier leaves, so the bloom check
     * reads a wrong byte range and rejects every candidate.
     *
     * The pg_tre.tuple_bloom_enable GUC bypasses tier-3 entirely until
     * pg_tre_posting_lookup_tuple_bloom learns to traverse the chain and
     * accumulate rank.  See doc/tier3-chain-rank.md.
     */
    if (result != NULL && st->q.global_max_cost >= 0 &&
        pg_tre_tuple_bloom_enable &&
        !tre_query_is_single_trigram(&st->q) &&
        sm_cardinality(result) <= (uint64) pg_tre_tier3_max_candidates)
    {
        result = apply_tuple_bloom_filter(scan->indexRelation, &st->q,
                                         result, st->scan_cxt);
    }

    /* Phase 5.1: positional filter.
     *
     * Same chain-rank limitation as tier-3 (see above): the positional
     * payload offset is computed from a per-leaf rank that does not
     * accumulate across right-link chains, so any TID past the first
     * leaf reads positions from the wrong slot.  Gated on the same GUC.
     *
     * Skip for DNF queries: tile alternatives store widened position
     * windows that are correct for tier-2 lookup but too loose to
     * filter individual TIDs without per-tile spine reconstruction.
     * The executor recheck (tre_match_scalar -> pg_tre_amatch) is
     * authoritative and runs on every candidate row regardless of
     * this filter, so skipping it here only loses an optimization,
     * not correctness.
     */
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

            /*
             * Iterate set bits via sm_next_member; the previous
             * walk over [sm_minimum, sm_maximum] called sm_contains
             * at every gap, which on a 100K-row 1000-page heap
             * iterated tens of millions of empty indexes.
             */
            while ((tid_idx = sm_next_member(result, tid_idx)) != SM_IDX_MAX)
            {
                    /*
                     * CNF: passes only if EVERY conjunct passes (start true,
                     * fail-fast on any failure).
                     * DNF: passes if ANY tile passes (start false, succeed-fast
                     * on any success).
                     */
                    bool passes = (st->q.mode == TRIGRAM_QUERY_CNF);
                    int ci, j;

                    /* For each conjunct/tile, check if required trigrams
                     * appear at valid positions */
                    for (ci = 0;
                         ci < st->q.n &&
                         (st->q.mode == TRIGRAM_QUERY_CNF ? passes : !passes);
                         ci++)
                    {
                        const TrigramConjunct *conj = &st->q.conjuncts[ci];

                        if (st->q.mode == TRIGRAM_QUERY_CNF)
                        {
                            /* CNF: at least one disjunct must have valid positions */
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
                            /*
                             * If no disjunct's trigram lives in the upper tree
                             * (e.g. all trigrams are pending-list-only after
                             * a recent INSERT), we couldn't evaluate the
                             * positional filter for this conjunct.  Treat it
                             * as a conservative pass so the candidate falls
                             * through to the executor recheck rather than
                             * being silently dropped.  This matches the
                             * tier-3 bloom branch's conservative-pass
                             * semantics for trigrams that are in the
                             * pending list but not yet flushed.
                             */
                            if (!any_evaluated)
                                conj_pass = true;
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
                    if (sm_add(filtered, tid_idx) == SM_IDX_MAX)
                    {
                        /* Overflow; give up on filtering */
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

done:
    /* Emit TIDs to the TIDBitmap. */
    if (result != NULL)
    {
        /*
         * O(cardinality) iteration via sm_next_member, replacing the
         * earlier O(span) scan that called sm_contains() for every
         * value between sm_minimum and sm_maximum.  On a 100K-row
         * test the old loop took minutes; this one returns in
         * milliseconds.
         */
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
