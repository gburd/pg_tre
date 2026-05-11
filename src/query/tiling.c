/*
 * src/query/tiling.c - Navarro pigeonhole k+1 tiling.
 *
 * For a pattern with min-length L and max edit cost k, partition the
 * pattern's required-trigram positions into k+1 disjoint tiles; at
 * least one tile must match exactly for the overall pattern to match
 * within cost k.  Emits an OR of per-tile conjunctions to the query
 * tree.
 *
 * Algorithm:
 *   Given a spine of n trigrams at positions p_0..p_{n-1} and budget k:
 *   1. Compute n_tiles = k + 1
 *   2. Partition spine into n_tiles roughly equal segments
 *   3. For each tile i, emit TrigramConjunct with AND of its trigrams
 *   4. Set TrigramQuery.mode = DNF (OR across tiles)
 *
 * Positional adjustment: each trigram's min_offset / max_offset is
 * widened by +/- k to account for insertions/deletions shifting positions.
 */

#include "postgres.h"

#include "utils/memutils.h"

#include "pg_tre/pg_tre.h"
#include "pg_tre/regex_ast.h"

/*
 * A spine entry: one trigram at a specific pattern offset.
 */
typedef struct SpineEntry
{
    uint8  trigram[3];
    int32  pattern_offset;
} SpineEntry;

/*
 * Extract the "trigram spine" from an AST: the sequence of all literal
 * trigrams in left-to-right order with their pattern offsets.  Returns
 * the number of spine entries, or -1 on overflow.
 */
static int
extract_spine(const RegexAst *ast, SpineEntry *out, int max_out,
              int32 *offset, MemoryContext cxt)
{
    int n = 0;

    if (ast == NULL)
        return 0;

    switch (ast->kind)
    {
        case REGEX_AST_LITERAL:
        {
            /* A single literal doesn't form a trigram; caller accumulates */
            (*offset)++;
            break;
        }

        case REGEX_AST_CONCAT:
        {
            /* Process left, then right */
            int n_left = extract_spine(ast->u.concat.left, out, max_out, offset, cxt);
            if (n_left < 0)
                return -1;
            n += n_left;
            if (n >= max_out)
                return -1;
            
            int n_right = extract_spine(ast->u.concat.right, &out[n], max_out - n, offset, cxt);
            if (n_right < 0)
                return -1;
            n += n_right;
            break;
        }

        case REGEX_AST_ALT:
        case REGEX_AST_REP:
        case REGEX_AST_APPROX:
        case REGEX_AST_ANY:
        case REGEX_AST_CLASS:
        case REGEX_AST_ANCHOR:
            /* These break the literal run; we don't extract from them */
            break;
    }

    return n;
}

/*
 * Linearize an AST into a byte string of literal characters, tracking
 * positions.  Returns the number of bytes written.  Non-literal nodes
 * (ANY, CLASS, etc.) are treated as opaque breaks.
 *
 * This is a helper for extracting the trigram spine from literal runs.
 */
static int
linearize_literals(const RegexAst *ast, uint8 *buf, int buf_cap, int *pos)
{
    int n = 0;

    if (ast == NULL)
        return 0;

    switch (ast->kind)
    {
        case REGEX_AST_LITERAL:
        {
            int32 cp = ast->u.literal.codepoint;
            if (cp >= 0 && cp <= 0xFF)
            {
                if (n >= buf_cap)
                    return -1;
                buf[n++] = (uint8) cp;
                (*pos)++;
            }
            break;
        }

        case REGEX_AST_CONCAT:
        {
            int n_left = linearize_literals(ast->u.concat.left, buf, buf_cap, pos);
            if (n_left < 0)
                return -1;
            n += n_left;

            int n_right = linearize_literals(ast->u.concat.right, &buf[n], buf_cap - n, pos);
            if (n_right < 0)
                return -1;
            n += n_right;
            break;
        }

        default:
            /* Non-literal node: break the run */
            break;
    }

    return n;
}

/*
 * Extract the spine more carefully: walk the AST, accumulate literal runs,
 * emit trigrams from each run >= 3 bytes.
 */
static int
extract_spine_from_ast(const RegexAst *ast, SpineEntry *out, int max_out,
                       MemoryContext cxt)
{
    uint8 buf[1024];
    int buf_len, i, n = 0;
    int32 pos = 0;

    buf_len = linearize_literals(ast, buf, 1024, &pos);
    if (buf_len < 0)
        return -1;

    /* Extract all trigrams from the linearized buffer */
    for (i = 0; i + 3 <= buf_len; i++)
    {
        if (n >= max_out)
            return -1;
        out[n].trigram[0] = buf[i];
        out[n].trigram[1] = buf[i + 1];
        out[n].trigram[2] = buf[i + 2];
        out[n].pattern_offset = i;
        n++;
    }

    return n;
}

/*
 * Tile a trigram spine into k+1 groups.  Each tile receives roughly
 * spine_n / (k+1) trigrams.  Returns the tiled TrigramQuery in DNF mode.
 */
bool
pg_tre_tile_spine(const SpineEntry *spine, int spine_n, int32 k,
                  TrigramQuery *out, MemoryContext cxt)
{
    MemoryContext old = MemoryContextSwitchTo(cxt);
    int n_tiles, tile_size, i, t;

    if (spine_n <= 0 || k < 0)
    {
        out->always_true = true;
        out->mode = TRIGRAM_QUERY_CNF;
        MemoryContextSwitchTo(old);
        return false;
    }

    n_tiles = k + 1;
    if (n_tiles > spine_n)
    {
        /* More tiles than trigrams; some tiles will be empty.
         * Treat as always_true since we can't guarantee any tile matches. */
        out->always_true = true;
        out->mode = TRIGRAM_QUERY_CNF;
        MemoryContextSwitchTo(old);
        return false;
    }

    tile_size = (spine_n + n_tiles - 1) / n_tiles;  /* ceiling division */

    out->n = n_tiles;
    out->conjuncts = palloc0(n_tiles * sizeof(TrigramConjunct));
    out->mode = TRIGRAM_QUERY_DNF;
    out->always_true = false;
    out->global_max_cost = k;
    out->min_match_len = 3;  /* conservative */

    for (t = 0; t < n_tiles; t++)
    {
        int tile_start = t * tile_size;
        int tile_end = tile_start + tile_size;
        if (tile_end > spine_n)
            tile_end = spine_n;

        int tile_len = tile_end - tile_start;
        if (tile_len <= 0)
            continue;

        out->conjuncts[t].n = tile_len;
        out->conjuncts[t].alts = palloc(tile_len * sizeof(TrigramDisjunct));

        for (i = 0; i < tile_len; i++)
        {
            const SpineEntry *e = &spine[tile_start + i];
            TrigramDisjunct *d = &out->conjuncts[t].alts[i];

            d->trigram_hash = pg_tre_hash_trigram(e->trigram);
            /* Widen position range by +/- k for edit distance tolerance */
            d->min_offset = (e->pattern_offset > k) ? (e->pattern_offset - k) : 0;
            d->max_offset = e->pattern_offset + k;
        }
    }

    MemoryContextSwitchTo(old);
    return true;
}

/*
 * Public entry point: given an AST and edit budget k, extract the spine
 * and tile it into k+1 groups.  Returns a DNF TrigramQuery.
 */
bool
pg_tre_tile_query(const RegexAst *ast, int32 k, TrigramQuery *out,
                  MemoryContext cxt)
{
    SpineEntry spine[1024];
    int spine_n;

    spine_n = extract_spine_from_ast(ast, spine, 1024, cxt);
    if (spine_n < 0)
    {
        out->always_true = true;
        out->mode = TRIGRAM_QUERY_CNF;
        return false;
    }

    return pg_tre_tile_spine(spine, spine_n, k, out, cxt);
}
