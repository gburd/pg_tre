/*
 * src/am/amcost.c - planner cost estimation (Phase 6).
 */

#include "postgres.h"

#include "access/amapi.h"
#include "catalog/pg_operator.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

#include "pg_tre/amapi.h"
#include "pg_tre/meta.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/regex_ast.h"

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

static Selectivity
estimate_trigram_selectivity(TrigramQuery *q, PgTreMetaPageData *meta)
{
	double mean_density;
	double sel;
	int i, j;

	if (q->always_true || q->n == 0)
		return 1.0;

	if (meta->n_tuples_indexed == 0)
		return 0.0001;

	if (meta->n_trigrams == 0)
	{
		mean_density = 0.0001;
	}
	else
	{
		mean_density = (double) meta->mean_posting_cardinality / meta->n_tuples_indexed;
		if (mean_density > 1.0)
			mean_density = 1.0;
		if (mean_density < 0.0001)
			mean_density = 0.0001;
	}

	if (q->mode == TRIGRAM_QUERY_CNF)
	{
		sel = 1.0;
		for (i = 0; i < q->n; i++)
		{
			const TrigramConjunct *conj = &q->conjuncts[i];
			double conj_sel = 0.0;

			for (j = 0; j < conj->n; j++)
			{
				conj_sel += mean_density;
				if (conj_sel >= 1.0)
				{
					conj_sel = 1.0;
					break;
				}
			}

			sel *= conj_sel;
		}
	}
	else
	{
		sel = 0.0;
		for (i = 0; i < q->n; i++)
		{
			const TrigramConjunct *tile = &q->conjuncts[i];
			double tile_sel = 1.0;

			for (j = 0; j < tile->n; j++)
			{
				tile_sel *= mean_density;
			}

			sel += tile_sel;
			if (sel >= 1.0)
			{
				sel = 1.0;
				break;
			}
		}
	}

	if (sel < 1.0 / meta->n_tuples_indexed)
		sel = 1.0 / meta->n_tuples_indexed;

	return sel;
}

void
pg_tre_amcostestimate(struct PlannerInfo *root, struct IndexPath *path,
                      double loop_count,
                      Cost *indexStartupCost, Cost *indexTotalCost,
                      Selectivity *indexSelectivity,
                      double *indexCorrelation, double *indexPages)
{
	Relation index = path->indexinfo->indexoid != InvalidOid
	                 ? index_open(path->indexinfo->indexoid, AccessShareLock)
	                 : NULL;
	PgTreMetaPageData meta;
	Selectivity sel = 0.01;
	Cost indexCost;
	double numIndexTuples;
	double numIndexPages;
	TrigramQuery q;
	bool have_query = false;

	(void) loop_count;

	if (index != NULL)
	{
		pg_tre_meta_read(index, &meta);

		if (path->indexclauses != NIL)
		{
			IndexClause *iclause = (IndexClause *) linitial(path->indexclauses);
			RestrictInfo *rinfo = iclause->rinfo;
			OpExpr *opexpr;
			Node *rightop;

			if (IsA(rinfo->clause, OpExpr))
			{
				opexpr = (OpExpr *) rinfo->clause;
				if (list_length(opexpr->args) == 2)
				{
					rightop = (Node *) lsecond(opexpr->args);
					if (IsA(rightop, Const) && !((Const *) rightop)->constisnull)
					{
						struct TrePatternData *pat;
						pat = (struct TrePatternData *) DatumGetPointer(((Const *) rightop)->constvalue);

						memset(&q, 0, sizeof(q));
						if (extract_query_from_pattern(pat, &q))
						{
							/* Prefer relation tuple count when meta is stale. */
							if (path->indexinfo->tuples > 0 &&
							    path->indexinfo->tuples >
							    (double) meta.n_tuples_indexed * 2.0)
							{
								meta.n_tuples_indexed = (uint64) path->indexinfo->tuples;
							}
							sel = estimate_trigram_selectivity(&q, &meta);
							have_query = true;
						}
					}
				}
			}
		}

		index_close(index, AccessShareLock);
	}

	if (have_query && q.always_true)
	{
		*indexStartupCost = disable_cost;
		*indexTotalCost = disable_cost;
		*indexSelectivity = 1.0;
		*indexCorrelation = 0.0;
		*indexPages = 0.0;
		return;
	}

	numIndexTuples = sel * path->indexinfo->tuples;
	if (numIndexTuples < 1.0)
		numIndexTuples = 1.0;

	if (have_query && q.n > 0)
	{
		int total_trigrams = 0;
		int i;

		for (i = 0; i < q.n; i++)
			total_trigrams += q.conjuncts[i].n;

		numIndexPages = total_trigrams * 10.0;

		if (index != NULL && numIndexPages > path->indexinfo->pages * 0.5)
			numIndexPages = path->indexinfo->pages * 0.5;
	}
	else
	{
		numIndexPages = path->indexinfo->pages * sel;
	}

	if (numIndexPages < 1.0)
		numIndexPages = 1.0;

	*indexStartupCost = 0.0;

	indexCost = numIndexPages * random_page_cost;
	indexCost += numIndexTuples * cpu_index_tuple_cost;

	if (have_query && q.global_max_cost > 0)
	{
		indexCost += numIndexTuples * cpu_operator_cost * 100.0;
	}
	else
	{
		indexCost += numIndexTuples * cpu_operator_cost * 10.0;
	}

	*indexTotalCost = *indexStartupCost + indexCost;
	*indexSelectivity = sel;
	*indexCorrelation = 0.0;
	*indexPages = numIndexPages;

	*indexTotalCost = *indexStartupCost + indexCost;
	*indexSelectivity = sel;
	*indexCorrelation = 0.0;
	*indexPages = numIndexPages;
}
