/*
 * src/am/sel.c - restriction selectivity for %~~ operator.
 *
 * Phase 6: tre_pattern_sel() estimates what fraction of rows will match
 * a regex pattern, allowing the planner to choose between index scan and
 * seq scan based on real cost estimates.
 */

#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "commands/defrem.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

#include "pg_tre/amapi.h"
#include "pg_tre/meta.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/posting.h"
#include "pg_tre/regex_ast.h"

#define DEFAULT_REGEX_SELECTIVITY 0.01

/*
 * Plan-time cap on posting-tree pages walked per trigram.  Past
 * this many TIDs a trigram is "common" and its exact count no
 * longer changes the plan decision; bounds I/O on hot trigrams.
 */
#define TRE_SEL_CARDINALITY_CAP 100000

PG_FUNCTION_INFO_V1(tre_pattern_sel);

static bool
extract_query_from_pattern(struct TrePatternData *pat, TrigramQuery *q_out)
{
	TreParseCtx ctx;
	char *pattern_str;
	int pattern_len;
	int32 max_cost;

	memset(&ctx, 0, sizeof(ctx));
	ctx.mcxt = CurrentMemoryContext;

	pattern_str = tre_pattern_get_text(pat, &pattern_len);
	max_cost = tre_pattern_get_max_cost(pat);

	if (!tre_parse_regex(&ctx, pattern_str, pattern_len))
		return false;

	if (!regex_extract_query(&ctx, max_cost, q_out))
		return false;

	return true;
}

/*
 * Per-trigram selectivity: fraction of indexed rows whose value
 * contains this trigram, from the trigram's posting-tree
 * cardinality.  Falls back to the global mean density when the
 * trigram is absent from the index (a literal run not present in
 * any row -> very selective) or the lookup fails.
 */
static double
trigram_selectivity(Relation index, uint64 trigram_hash,
                    double mean_density, uint64 n_tuples)
{
    PgTreUpperRef ref;
    uint64        card;
    double        sel;

    if (n_tuples == 0)
        return mean_density;

    if (!pg_tre_upper_lookup(index, trigram_hash, &ref))
    {
        /*
         * Trigram not in the index at all: no row contains it, so
         * this required trigram makes the whole pattern match (at
         * most) the floor.  Return a very small selectivity.
         */
        return 1.0 / (double) n_tuples;
    }

    card = pg_tre_posting_cardinality(index, ref.root, ref.inline_data,
                                     ref.inline_bytes,
                                     TRE_SEL_CARDINALITY_CAP);
    pg_tre_upper_release(&ref);

    sel = (double) card / (double) n_tuples;
    if (sel > 1.0)
        sel = 1.0;
    if (sel < 1.0 / (double) n_tuples)
        sel = 1.0 / (double) n_tuples;
    return sel;
}

/* OR (union) selectivity of a conjunct's alternative trigrams,
 * under independence: 1 - prod(1 - sel_i). */
static double
conjunct_or_selectivity(Relation index, const TrigramConjunct *conj,
                        double mean_density, uint64 n_tuples)
{
    double  not_any = 1.0;
    int     j;

    if (conj->n == 0)
        return 1.0;   /* empty alternation matches everything */

    for (j = 0; j < conj->n; j++)
    {
        double s = trigram_selectivity(index, conj->alts[j].trigram_hash,
                                       mean_density, n_tuples);
        not_any *= (1.0 - s);
    }
    return 1.0 - not_any;
}

Selectivity
pg_tre_estimate_trigram_selectivity(Relation index, TrigramQuery *q,
                                    PgTreMetaPageData *meta)
{
    double mean_density;
    double sel;
    uint64 n_tuples = meta->n_tuples_indexed;
    int i;

    if (q->always_true || q->n == 0)
        return 1.0;

    if (n_tuples == 0)
        return 0.0001;

    /* Fallback global density when a per-trigram lookup can't help. */
    if (meta->n_trigrams == 0)
        mean_density = 0.01;
    else if (meta->mean_posting_cardinality == 0)
    {
        mean_density = 1.0 / (double) meta->n_trigrams;
        if (mean_density < 0.0001)
            mean_density = 0.0001;
    }
    else
    {
        mean_density = (double) meta->mean_posting_cardinality / (double) n_tuples;
        if (mean_density > 1.0)
            mean_density = 1.0;
        if (mean_density < 0.0001)
            mean_density = 0.0001;
    }

    if (q->mode == TRIGRAM_QUERY_CNF)
    {
        /*
         * AND of conjuncts (each an OR of alternative trigrams).
         *
         * Trigrams from one literal run co-occur perfectly: a row
         * that contains "common_tok" contains ALL of its trigrams.
         * A pure independence product therefore wildly
         * under-estimates -- 8 trigrams each at 0.5 would give
         * 0.5^8, not the true 0.5.  We model the AND as the
         * *minimum* conjunct selectivity (the rarest required
         * trigram bounds how many rows can match) with a mild
         * geometric dampening so a longer literal is estimated a
         * bit more selective than its single rarest trigram, but
         * never exponentially so.
         */
        double min_sel = 1.0;
        double prod = 1.0;
        int    counted = 0;

        for (i = 0; i < q->n; i++)
        {
            double conj_sel = conjunct_or_selectivity(index, &q->conjuncts[i],
                                                      mean_density, n_tuples);
            if (conj_sel < min_sel)
                min_sel = conj_sel;
            prod *= conj_sel;
            counted++;
        }

        if (counted == 0)
            sel = 1.0;
        else
        {
            /*
             * Trigrams from one literal co-occur, so a row matches
             * the AND essentially iff it contains the rarest
             * required trigram: the rarest trigram's selectivity is
             * the estimate.  (A pure independence product would
             * under-estimate by orders of magnitude; min_sel is the
             * tight, correct bound for perfectly-correlated
             * trigrams.)  We never under-estimate below the
             * independence product as a floor.
             */
            sel = min_sel;
            if (sel < prod)
                sel = prod;
        }
    }
    else
    {
        /*
         * DNF: OR of tiles, produced by k>0 edit-distance tiling.
         * Each tile is the set of fuzzy trigram variants that can
         * appear at the tile's pattern positions; a row matches the
         * query if any tile matches.  The tiles' matched row sets
         * overlap heavily (they are shifted/edited views of the
         * same pattern), and within a tile the many edit-distance
         * variants form an OR (any-one-present satisfies a
         * position).  A full independence union across all variants
         * and tiles saturates to the whole table for even moderate
         * k, which is wrong.
         *
         * We estimate conservatively as the maximum per-tile union
         * selectivity, then inflate by a sub-linear factor of the
         * tile count and clamp.  This keeps a k>0 estimate a bounded
         * multiple of the underlying selectivity -- selective enough
         * patterns still favor the index, very fuzzy ones (large k,
         * short pattern) tend toward a seq scan, which matches how
         * the index actually behaves (tiling defeats pruning).
         */
        double max_tile = 0.0;
        int    ntiles = 0;

        for (i = 0; i < q->n; i++)
        {
            /* Treat the tile's alternatives as an OR (union). */
            double tile_sel = conjunct_or_selectivity(index, &q->conjuncts[i],
                                                      mean_density, n_tuples);
            if (tile_sel > max_tile)
                max_tile = tile_sel;
            ntiles++;
        }

        if (ntiles == 0)
            sel = mean_density;
        else
        {
            double inflate = 1.0 + 0.5 * log((double) ntiles + 1.0);
            sel = max_tile * inflate;
            if (sel > 0.5)
                sel = 0.5;   /* a fuzzy query is never "super-selective" */
        }
    }

    if (sel < 1.0 / (double) n_tuples)
        sel = 1.0 / (double) n_tuples;
    if (sel > 1.0)
        sel = 1.0;

    return sel;
}

Datum
tre_pattern_sel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	List *args = (List *) PG_GETARG_POINTER(2);
	int varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node *other;
	bool varonleft;
	Selectivity sel;
	struct TrePatternData *pat;
	TrigramQuery q;
	Oid indexoid;
	Relation index;
	PgTreMetaPageData meta;

	(void) root;
	(void) varRelid;

	if (!get_restriction_variable(root, args, varRelid,
	                              &vardata, &other, &varonleft))
	{
		PG_RETURN_FLOAT8(DEFAULT_REGEX_SELECTIVITY);
	}

	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_REGEX_SELECTIVITY);
	}

	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}

	pat = (struct TrePatternData *) DatumGetPointer(((Const *) other)->constvalue);

	memset(&q, 0, sizeof(q));
	if (!extract_query_from_pattern(pat, &q))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_REGEX_SELECTIVITY);
	}

	indexoid = InvalidOid;
	if (vardata.rel)
	{
		ListCell *lc;
		foreach(lc, vardata.rel->indexlist)
		{
			IndexOptInfo *indexinfo = (IndexOptInfo *) lfirst(lc);
			if (indexinfo->relam == InvalidOid)
				continue;

			{
				char *amname = get_am_name(indexinfo->relam);
				if (amname && strcmp(amname, "tre") == 0)
				{
					indexoid = indexinfo->indexoid;
					break;
				}
			}
		}
	}

	if (indexoid == InvalidOid)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_REGEX_SELECTIVITY);
	}

	index = index_open(indexoid, AccessShareLock);
	pg_tre_meta_read(index, &meta);

	/*
	 * If the metapage stats are stale (indexed tuple count is much less
	 * than the relation's current tuple count), prefer the relation stats.
	 * This matters after INSERT + VACUUM cycles where pending merge
	 * updates n_tuples_indexed only approximately.
	 */
	if (vardata.rel && vardata.rel->tuples > 0 &&
	    vardata.rel->tuples > (double) meta.n_tuples_indexed * 2.0)
	{
		meta.n_tuples_indexed = (uint64) vardata.rel->tuples;
	}

	/*
	 * A3: estimate using real per-trigram posting cardinalities read
	 * from the index, so common trigrams cost as common and rare
	 * trigrams as rare.  The index stays open across the estimate
	 * because the per-trigram lookups read its pages.
	 */
	sel = pg_tre_estimate_trigram_selectivity(index, &q, &meta);
	index_close(index, AccessShareLock);

	ReleaseVariableStats(vardata);
	PG_RETURN_FLOAT8((float8) sel);
}
