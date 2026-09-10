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

#include "miscadmin.h"
#include "utils/memutils.h"

#include "pg_tre/hash.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/regex_ast.h"
#include "pg_tre/tiling.h"
#include "pg_tre/uleven.h"
#include "pg_tre/utf8.h"

/*
 * Accumulator for building the required-trigram set.  We collect unique
 * trigrams (by their hash + codepoint representation) into a dynamic array.
 * For simple patterns, the set is small (<= dozens), so linear dedup is
 * fine.
 *
 * Phase 3.5: codepoint-based trigrams (3 x int32) instead of byte trigrams.
 */
typedef struct TrigramAccum
{
    int32   (*tris)[3];     /* array of 3-codepoint trigrams */
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
accum_add(TrigramAccum *a, const int32 t[3], MemoryContext cxt)
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
 * track a "literal run" buffer of codepoints, flush to the accumulator on every
 * break point.
 *
 * Phase 3.5: run buffer contains int32 codepoints, not bytes.
 */
typedef struct LinCtx
{
    int32      *run;    /* array of codepoints */
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
    lc->run = lc->run ? repalloc(lc->run, lc->run_cap * sizeof(int32))
                      : palloc(lc->run_cap * sizeof(int32));
    MemoryContextSwitchTo(old);
}

static void
run_append_cp(LinCtx *lc, int32 cp)
{
    if (lc->run_n >= lc->run_cap)
        run_grow(lc);
    lc->run[lc->run_n++] = cp;
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

    /*
     * Bound recursion against the C stack: a pathologically nested
     * pattern (deeply parenthesized concat/alt/rep) would otherwise
     * recurse until the backend segfaults.  check_stack_depth() raises
     * a clean ereport before that happens.
     */
    check_stack_depth();

    switch (ast->kind)
    {
        case REGEX_AST_LITERAL:
            /*
             * Phase 3.5: append the codepoint directly to the run.
             * We support all Unicode codepoints (0x0000..0x10FFFF), not just ASCII.
             */
            run_append_cp(lc, ast->u.literal.codepoint);
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
 *
 * Phase 3.5: use pg_tre_hash_trigram_cp for codepoint trigrams.
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
        c->alts[0].trigram_hash = pg_tre_hash_trigram_cp(a->tris[i]);
        c->alts[0].min_offset = 0;
        c->alts[0].max_offset = INT32_MAX;
    }

    MemoryContextSwitchTo(old);
}

/*
 * Detect a required anchored leading literal and, if present, derive the
 * closed trigram-key range its first trigram must occupy.  Returns true and
 * fills lo/hi when the pattern begins with `^` (REGEX_ANCHOR_START)
 * immediately followed by >= 3 literal codepoints; those three codepoints
 * form an exact order-preserving key K, so the range is [K, K].
 *
 * We only walk the leftmost CONCAT spine and only accept LITERAL nodes, so
 * this fires exactly for patterns like `^foobar`, `^error`, and the
 * anchored-prefix form that LIKE 'foo%' lowers to.  Anything else (no
 * anchor, a class/any/rep/alt before three literals, fewer than three
 * leading literals) yields false and no SuRF range -- correctness is never
 * affected, only the opportunity to prefilter.
 *
 * Soundness: for an anchored k=0 pattern, every matching string starts with
 * these codepoints, so its first indexed trigram hashes/keys to exactly K.
 * If the index contains no trigram with key K, no row can match -> safe to
 * reject.  (At k>0 the leading bytes may be edited, so we do NOT set a range
 * there; the tiling path leaves has_surf_range false.)
 */
static bool
extract_anchored_prefix_key(const RegexAst *root, uint64 *lo, uint64 *hi)
{
    const RegexAst *node = root;
    int32       cps[3];
    int         ncp = 0;
    bool        seen_anchor = false;

    /*
     * Walk the leftmost spine.  A pattern `^abc...` parses as nested
     * CONCAT with the anchor as the deepest-left leaf, so we descend
     * left children, visiting nodes in left-to-right order via an
     * explicit stack of "pending right children".
     */
    const RegexAst *stack[64];
    int         sp = 0;

    for (;;)
    {
        if (node == NULL)
        {
            if (sp == 0)
                break;
            node = stack[--sp];
            continue;
        }

        switch (node->kind)
        {
            case REGEX_AST_CONCAT:
                /* visit left, then right */
                if (sp < 64)
                    stack[sp++] = node->u.concat.right;
                node = node->u.concat.left;
                continue;

            case REGEX_AST_ANCHOR:
                if (node->u.anchor.which == REGEX_ANCHOR_START && ncp == 0)
                    seen_anchor = true;
                else if (ncp < 3)
                    return false;   /* mid-run anchor: not a clean prefix */
                node = NULL;
                continue;

            case REGEX_AST_LITERAL:
                if (!seen_anchor)
                    return false;   /* not anchored: no hard prefix */
                if (ncp < 3)
                    cps[ncp] = node->u.literal.codepoint;
                ncp++;
                if (ncp >= 3)
                {
                    /* Got three leading literals: exact first-trigram key. */
                    uint64 k = pg_tre_trigram_key_cp(cps);

                    *lo = k;
                    *hi = k;
                    return true;
                }
                node = NULL;
                continue;

            default:
                /* Any non-literal before 3 literals breaks the prefix. */
                return false;
        }
    }
    return false;
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
         * into k+1 tiles; at least one must match exactly.
         * Phase 5.1: tiling now includes uleven expansion per tile. */
        if (pg_tre_tile_query(ctx->root, max_cost, out, ctx->mcxt))
        {
            /* Tiling succeeded with uleven expansion */
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

    /*
     * v10 SuRF prefilter: if the pattern pins an anchored >= 3-codepoint
     * leading literal, record the exact first-trigram key range so the
     * scan can reject the whole index when no such trigram exists.  Only
     * at k=0 (leading bytes are not editable).
     *
     * Never claim a range when the extraction is already always_true: the
     * recheck is then the only authority on matching, and the key range --
     * derived from raw literal codepoints with NO case folding -- may not
     * describe what the index legitimately stores.  amscan's prefilter
     * enforces this too; both sides matter, because a range that exists is
     * a range some future caller may consult.
     */
    if (max_cost == 0 && !out->always_true)
    {
        uint64  lo,
                hi;

        if (extract_anchored_prefix_key(ctx->root, &lo, &hi))
        {
            out->has_surf_range = true;
            out->surf_lo = lo;
            out->surf_hi = hi;
        }
    }

    return true;
}
