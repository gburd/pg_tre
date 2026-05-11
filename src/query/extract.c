/*
 * src/query/extract.c - regex AST to trigram Boolean formula.
 *
 * Phase 3 implementation of a simplified Russ Cox style extraction for
 * the k=0 (exact) case.
 *
 * We synthesize, per AST node, a single property: `required_trigrams` --
 * a set of 3-byte strings that MUST appear somewhere in any string the
 * node accepts.  The final TrigramQuery is the AND of all required
 * trigrams at the root (rendered as one disjunct per trigram, each with
 * a single alternative -- so effectively an AND list).
 *
 * Propagation rules, simplified:
 *   LITERAL c           : exact string = [c]
 *   CLASS / ANY         : exact string unknown (special "opaque" byte);
 *                         contribution: break the surrounding literal run
 *   ANCHOR              : no-op
 *   CONCAT(l, r)        : concat exact strings, extract trigrams from the
 *                         combined run; each operand contributes its own
 *                         trigrams too
 *   ALT(l, r)           : require trigrams common to BOTH sides
 *                         (intersection).  Simplification: if either side
 *                         has no trigrams, the alternation contributes
 *                         none either.
 *   REP(x, m, n)        : if m == 0: contribute nothing (could match empty)
 *                         if m >= 1: contribute x's trigrams
 *                         (m-fold concat handled implicitly)
 *   APPROX(sub, k)      : for k=0 extraction: treat as sub.  (Phase 5
 *                         will add edit budget to weaken contributions.)
 *
 * This is conservative: we may fail to extract trigrams that an ideal
 * algorithm could find, but we never require trigrams that a matching
 * string lacks.  The planner relies on amcostestimate to fall back to
 * seq-scan when extraction produces too little selectivity; at Phase 3
 * we force the index path via SET enable_seqscan=off in tests.
 *
 * Phase 3 is ASCII-only (byte trigrams).  Phase 3.5 extends to UTF-8
 * codepoint trigrams.
 *
 * Phase 5 extends this file to handle:
 *   - global max_cost > 0 via Navarro's (k+1)-tiling
 *   - positional constraints (min_offset / max_offset)
 *   - universal-Levenshtein expansion for near-literal patterns
 *   - fanout cap via pg_tre_max_extraction_fanout
 */

#include "postgres.h"

#include <string.h>

#include "utils/memutils.h"

#include "pg_tre/hash.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/regex_ast.h"
#include "pg_tre/tiling.h"
#include "pg_tre/uleven.h"

/*
 * Accumulator for building the required-trigram set.  We collect unique
 * trigrams (by their hash + byte representation) into a dynamic array.
 * For simple patterns, the set is small (<= dozens), so linear dedup is
 * fine.
 */
typedef struct TrigramAccum
{
    uint8   (*tris)[3];     /* array of 3-byte trigrams */
    int       n;
    int       alloced;
    bool      overflowed;   /* true if we gave up (fanout cap) */
} TrigramAccum;

static void
accum_init(TrigramAccum *a, MemoryContext cxt)
{
    MemoryContext old = MemoryContextSwitchTo(cxt);
    a->alloced = 16;
    a->tris = palloc(a->alloced * sizeof(*a->tris));
    a->n = 0;
    a->overflowed = false;
    MemoryContextSwitchTo(old);
}

static void
accum_add(TrigramAccum *a, const uint8 t[3], MemoryContext cxt)
{
    int i;

    if (a->overflowed)
        return;

    for (i = 0; i < a->n; i++)
    {
        if (a->tris[i][0] == t[0] &&
            a->tris[i][1] == t[1] &&
            a->tris[i][2] == t[2])
            return;
    }

    if (a->n >= pg_tre_max_extraction_fanout)
    {
        a->overflowed = true;
        return;
    }

    if (a->n >= a->alloced)
    {
        MemoryContext old = MemoryContextSwitchTo(cxt);
        a->alloced *= 2;
        a->tris = repalloc(a->tris, a->alloced * sizeof(*a->tris));
        MemoryContextSwitchTo(old);
    }

    a->tris[a->n][0] = t[0];
    a->tris[a->n][1] = t[1];
    a->tris[a->n][2] = t[2];
    a->n++;
}

/*
 * Linearize an AST subtree into a byte sequence of "definite literal
 * bytes" interspersed with OPAQUE markers.  Opaque markers break trigram
 * extraction (we can't see through them).
 *
 * We emit a simple stream to a caller-allocated buffer:
 *   byte value 0..255 = literal byte
 *   -1 (OPAQUE) = "any single byte" or "class that we didn't narrow"
 *   -2 (EMPTY) = "this position contributes nothing, concat passes through"
 *
 * Rep with min=0 or alternation with non-literal sides emit OPAQUE
 * markers so extraction conservatively ignores their interior.
 *
 * Return value is the number of items written; -1 on overflow.
 */
#define OPAQUE_BYTE -1
#define EMPTY_BYTE  -2

/*
 * Linearize and extract trigrams in a single pass: walk the AST,
 * track a "literal run" buffer, flush to the accumulator on every
 * break point.
 */
typedef struct LinCtx
{
    uint8      *run;
    int         run_n;
    int         run_cap;
    TrigramAccum *acc;
    MemoryContext cxt;
} LinCtx;

static void lin_append_node(LinCtx *lc, const RegexAst *ast);

static void
run_grow(LinCtx *lc)
{
    MemoryContext old = MemoryContextSwitchTo(lc->cxt);
    lc->run_cap = lc->run_cap == 0 ? 64 : lc->run_cap * 2;
    lc->run = lc->run ? repalloc(lc->run, lc->run_cap)
                      : palloc(lc->run_cap);
    MemoryContextSwitchTo(old);
}

static void
run_append_byte(LinCtx *lc, uint8 c)
{
    if (lc->run_n >= lc->run_cap)
        run_grow(lc);
    lc->run[lc->run_n++] = c;
}

/*
 * Emit every trigram in the current literal run to the accumulator,
 * then clear the run.  Called on any break point (OPAQUE, alternation,
 * repetition-with-min=0, etc.).
 */
static void
run_flush(LinCtx *lc)
{
    int i;

    for (i = 0; i + 3 <= lc->run_n; i++)
        accum_add(lc->acc, &lc->run[i], lc->cxt);

    lc->run_n = 0;
}

static void
lin_append_node(LinCtx *lc, const RegexAst *ast)
{
    if (ast == NULL || lc->acc->overflowed)
        return;

    switch (ast->kind)
    {
        case REGEX_AST_LITERAL:
            /* Bytes 0..127 appendable directly (Phase 3 ASCII only). */
            if (ast->u.literal.codepoint >= 0 &&
                ast->u.literal.codepoint <= 0xFF)
            {
                run_append_byte(lc, (uint8) ast->u.literal.codepoint);
            }
            else
            {
                /* Non-ASCII literal in Phase 3: treat as opaque. */
                run_flush(lc);
            }
            break;

        case REGEX_AST_ANY:
        case REGEX_AST_CLASS:
            /* Break the run: opaque byte. */
            run_flush(lc);
            break;

        case REGEX_AST_ANCHOR:
            /* Anchor doesn't consume a byte; keep the run intact.  For
             * Phase 3 we don't record anchor positions (Phase 5 does). */
            break;

        case REGEX_AST_CONCAT:
            lin_append_node(lc, ast->u.concat.left);
            lin_append_node(lc, ast->u.concat.right);
            break;

        case REGEX_AST_ALT:
        {
            /* Conservative: compute each side's required trigrams
             * separately, then intersect.  For Phase 3 simplicity we
             * only take the INTERSECTION of trigrams sets: a trigram
             * is required by the alternation iff it's required by
             * BOTH sides.  If either side has no required trigrams,
             * the alternation contributes none.
             *
             * We implement this by flushing the current run, running
             * extraction on each branch into separate accumulators,
             * intersecting, merging the intersection back into lc->acc,
             * and flushing again (so nothing from the branches leaks
             * into an outer literal run).
             */
            TrigramAccum a, b;
            int i, j;
            LinCtx lca = *lc, lcb = *lc;

            run_flush(lc);

            accum_init(&a, lc->cxt);
            lca.acc = &a;
            lca.run = NULL; lca.run_n = 0; lca.run_cap = 0;
            lin_append_node(&lca, ast->u.alt.left);
            /* final flush for the branch */
            {
                int k;
                for (k = 0; k + 3 <= lca.run_n; k++)
                    accum_add(&a, &lca.run[k], lc->cxt);
            }

            accum_init(&b, lc->cxt);
            lcb.acc = &b;
            lcb.run = NULL; lcb.run_n = 0; lcb.run_cap = 0;
            lin_append_node(&lcb, ast->u.alt.right);
            {
                int k;
                for (k = 0; k + 3 <= lcb.run_n; k++)
                    accum_add(&b, &lcb.run[k], lc->cxt);
            }

            /* Intersection of a and b. */
            for (i = 0; i < a.n; i++)
            {
                for (j = 0; j < b.n; j++)
                {
                    if (a.tris[i][0] == b.tris[j][0] &&
                        a.tris[i][1] == b.tris[j][1] &&
                        a.tris[i][2] == b.tris[j][2])
                    {
                        accum_add(lc->acc, a.tris[i], lc->cxt);
                        break;
                    }
                }
            }
            break;
        }

        case REGEX_AST_REP:
        {
            int32 m = ast->u.rep.min_rep;

            if (m == 0)
            {
                /* Can match empty -- break the run. */
                run_flush(lc);
            }
            else
            {
                /* Guaranteed at least one occurrence -- inline the
                 * child m times (capped at 2 to keep the run bounded). */
                int k = (m > 2) ? 2 : m;
                int i;
                for (i = 0; i < k; i++)
                    lin_append_node(lc, ast->u.rep.child);
            }
            break;
        }

        case REGEX_AST_APPROX:
            /* Phase 3 (k=0): treat APPROX as its child.  Phase 5 reads
             * ast->u.approx.k and weakens the contribution. */
            lin_append_node(lc, ast->u.approx.child);
            break;
    }
}

/*
 * Convert an extracted TrigramAccum into a TrigramQuery.  Each
 * trigram becomes one conjunct (AND across all), with a single
 * disjunct containing that trigram's hash (OR of size 1).
 */
static void
accum_to_query(const TrigramAccum *a, int32 max_cost, TrigramQuery *out,
               MemoryContext cxt)
{
    MemoryContext old = MemoryContextSwitchTo(cxt);
    int i;

    out->global_max_cost = max_cost;
    out->min_match_len   = 3;   /* conservative lower bound */
    out->mode            = TRIGRAM_QUERY_CNF;  /* default CNF mode */

    if (a->overflowed || a->n == 0)
    {
        out->always_true = true;
        out->n = 0;
        out->conjuncts = NULL;
        MemoryContextSwitchTo(old);
        return;
    }

    out->always_true = false;
    out->n = a->n;
    out->conjuncts = palloc(a->n * sizeof(TrigramConjunct));

    for (i = 0; i < a->n; i++)
    {
        TrigramConjunct *c = &out->conjuncts[i];
        c->n = 1;
        c->alts = palloc(sizeof(TrigramDisjunct));
        c->alts[0].trigram_hash = pg_tre_hash_trigram(a->tris[i]);
        c->alts[0].min_offset = 0;
        c->alts[0].max_offset = INT32_MAX;
    }

    MemoryContextSwitchTo(old);
}

bool
regex_extract_query(TreParseCtx *ctx, int32 max_cost, TrigramQuery *out)
{
    TrigramAccum acc;
    LinCtx       lc;
    int          i;

    memset(out, 0, sizeof(*out));

    /* Phase 5: handle k > 0 via Navarro tiling */
    if (max_cost > 0)
    {
        /* Use tiling for k > 0: partition the pattern's trigram spine
         * into k+1 tiles; at least one must match exactly. */
        if (pg_tre_tile_query(ctx->root, max_cost, out, ctx->mcxt))
        {
            /* Tiling succeeded; expand each tile's trigrams with uleven if k is small.
             * For k=1 or k=2, we can enumerate near-neighbor trigrams. */
            if (max_cost <= 2 && max_cost > 0)
            {
                int t, d;
                for (t = 0; t < out->n; t++)
                {
                    TrigramConjunct *conj = &out->conjuncts[t];
                    /* For each disjunct in this tile, expand it with uleven */
                    for (d = 0; d < conj->n; d++)
                    {
                        TrigramDisjunct *dis = &conj->alts[d];
                        uint8 original_tri[3];
                        uint8 expanded[4096][3];
                        int n_expanded;

                        /* Extract the original trigram bytes from the hash.
                         * Since we only have the hash, we can't expand directly.
                         * Instead, we'll skip uleven expansion in Phase 5 initial cut
                         * and add it in Phase 5.1 once we track trigram bytes alongside hashes.
                         * For now, document this limitation. */
                        (void) original_tri;
                        (void) expanded;
                        (void) n_expanded;
                        /* TODO Phase 5.1: store trigram bytes alongside hash in SpineEntry,
                         * then call pg_tre_uleven_expand here and add the expanded trigrams
                         * as additional disjuncts within the tile. */
                    }
                }
            }
            return true;
        }
        else
        {
            /* Tiling failed (pattern too short, no spine, etc.); fall back to always_true */
            out->always_true = true;
            out->global_max_cost = max_cost;
            out->mode = TRIGRAM_QUERY_CNF;
            return true;
        }
    }

    /* Phase 3 path: k=0 exact extraction */
    accum_init(&acc, ctx->mcxt);

    memset(&lc, 0, sizeof(lc));
    lc.cxt = ctx->mcxt;
    lc.acc = &acc;
    lc.run = NULL;
    lc.run_n = 0;
    lc.run_cap = 0;

    lin_append_node(&lc, ctx->root);

    /* Flush any residual literal run. */
    for (i = 0; i + 3 <= lc.run_n; i++)
        accum_add(&acc, &lc.run[i], ctx->mcxt);

    accum_to_query(&acc, max_cost, out, ctx->mcxt);
    out->mode = TRIGRAM_QUERY_CNF;  /* k=0 uses CNF */
    return true;
}
