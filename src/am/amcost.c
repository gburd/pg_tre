/*
 * src/am/amcost.c - planner cost estimation (Phase 6).
 */

#include "postgres.h"

#include <math.h>

#include "access/amapi.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "varatt.h"

#include "pg_tre/amapi.h"
#include "pg_tre/like_translate.h"
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

/*
 * Lower an already-translated regex string (from a LIKE/literal/regex
 * RHS, Phase A/A1) into a TrigramQuery at k=0, for cost estimation.
 */
static bool
extract_query_from_text(const char *regex, int len, TrigramQuery *q_out)
{
	TreParseCtx ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.mcxt = CurrentMemoryContext;

	if (!tre_parse_regex(&ctx, regex, len))
		return false;
	if (!regex_extract_query(&ctx, 0, q_out))
		return false;
	return true;
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
						/*
						 * Dispatch on the RHS constant type.  Strategy 1
						 * carries a tre_pattern; the A1 parity operators
						 * carry a plain text RHS and must be lowered via
						 * the LIKE/literal-to-regex translation, not
						 * reinterpreted as a tre_pattern struct.
						 */
						Const *rconst = (Const *) rightop;
						Oid    rtype = rconst->consttype;

						memset(&q, 0, sizeof(q));

						if (rtype == TEXTOID)
						{
							/*
							 * A1 parity operator.  Recover the operator's
							 * strategy to translate correctly; default to
							 * LIKE-style if unknown (harmless for a cost
							 * estimate -- it only affects selectivity).
							 */
							text   *rhs = DatumGetTextPP(rconst->constvalue);
							char   *raw = VARDATA_ANY(rhs);
							int     rawlen = VARSIZE_ANY_EXHDR(rhs);
							char   *rx;
							StrategyNumber strat =
								get_op_opfamily_strategy(opexpr->opno,
								                         path->indexinfo->opfamily[0]);

							if (strat == PG_TRE_STRATEGY_EQUAL)
								rx = pg_tre_literal_to_regex(raw, rawlen);
							else if (strat == PG_TRE_STRATEGY_REGEX ||
							         strat == PG_TRE_STRATEGY_IREGEX)
							{
								rx = (char *) palloc(rawlen + 1);
								memcpy(rx, raw, rawlen);
								rx[rawlen] = '\0';
							}
							else   /* LIKE / ILIKE / unknown */
								rx = pg_tre_like_to_regex(raw, rawlen, '\\');

							if (extract_query_from_text(rx, (int) strlen(rx), &q))
							{
								if (path->indexinfo->tuples > 0 &&
								    path->indexinfo->tuples >
								    (double) meta.n_tuples_indexed * 2.0)
									meta.n_tuples_indexed =
										(uint64) path->indexinfo->tuples;
								sel = pg_tre_estimate_trigram_selectivity(index, &q, &meta);
								have_query = true;
							}
						}
						else
						{
							struct TrePatternData *pat;
							pat = (struct TrePatternData *) DatumGetPointer(rconst->constvalue);

							if (extract_query_from_pattern(pat, &q))
							{
								/* Prefer relation tuple count when meta is stale. */
								if (path->indexinfo->tuples > 0 &&
								    path->indexinfo->tuples >
								    (double) meta.n_tuples_indexed * 2.0)
								{
									meta.n_tuples_indexed = (uint64) path->indexinfo->tuples;
								}
								sel = pg_tre_estimate_trigram_selectivity(index, &q, &meta);
								have_query = true;
							}
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

	/*
	 * Realistic cost model:
	 *   - Upper tree: 1-2 pages for small indexes (log base ~100 fanout)
	 *   - Per-trigram posting: 1 page (most postings inline or single leaf)
	 *   - AND/OR merge: CPU cost proportional to TID count
	 *   - Regex recheck: expensive (20x cpu_operator_cost per tuple)
	 */
	if (have_query && q.n > 0)
	{
		int total_trigrams = 0;
		int i;
		double num_upper_pages;
		double num_posting_pages;
		double avg_pages_per_posting;

		for (i = 0; i < q.n; i++)
			total_trigrams += q.conjuncts[i].n;

		/* Upper tree height: log base 100 of total distinct trigrams.
		 * For small indexes (<100 trigrams), tree is 1 page.
		 * For 100-10k trigrams, tree is 2 pages.
		 * Use max(1.0, ...) to avoid log domain errors. */
		if (meta.n_trigrams > 100)
		{
			double log_factor = log((double) meta.n_trigrams) / log(100.0);
			num_upper_pages = 1.0 + ceil(log_factor);
		}
		else
		{
			num_upper_pages = 1.0;
		}

		/* Posting pages: estimate via meta stats or fallback heuristic.
		 * Most postings for selective patterns fit inline or in 1 page.
		 * Use mean_posting_cardinality if available, else assume
		 * avg posting size ~ n_tuples / n_trigrams (uniform distribution). */
		if (meta.mean_posting_cardinality > 0)
		{
			/* Use recorded stats: bytes per posting / page size. */
			avg_pages_per_posting = (double) meta.mean_posting_cardinality / 
			                        (BLCKSZ / 8);  /* rough: 1 bit per TID */
		}
		else if (meta.n_trigrams > 0)
		{
			/* Fallback: assume uniform distribution of trigrams. */
			double avg_tids_per_trigram = (double) meta.n_tuples_indexed / 
			                              (double) meta.n_trigrams;
			/* Inline threshold is ~256 bytes = ~2000 TIDs in sparsemap.
			 * If avg < 500 TIDs, postings are mostly inline (0 extra pages).
			 * Otherwise, 1 page per posting is typical. */
			if (avg_tids_per_trigram < 500.0)
				avg_pages_per_posting = 0.5;  /* half inline, half 1-page */
			else
				avg_pages_per_posting = 1.0;
		}
		else
		{
			/* No stats at all: assume mostly inline. */
			avg_pages_per_posting = 0.5;
		}

		num_posting_pages = total_trigrams * avg_pages_per_posting;

		/* Cap at half the index size (prevent absurd estimates). */
		numIndexPages = num_upper_pages + num_posting_pages;
		if (index != NULL && numIndexPages > path->indexinfo->pages * 0.5)
			numIndexPages = path->indexinfo->pages * 0.5;
	}
	else
	{
		numIndexPages = path->indexinfo->pages * sel;
	}

	if (numIndexPages < 1.0)
		numIndexPages = 1.0;

	/* Startup cost: upper tree lookup per trigram.
	 * Use reduced random_page_cost (often cached after first lookup). */
	if (have_query && q.n > 0)
	{
		int total_trigrams = 0;
		int i;
		for (i = 0; i < q.n; i++)
			total_trigrams += q.conjuncts[i].n;

		*indexStartupCost = total_trigrams * random_page_cost * 0.5;
	}
	else
	{
		*indexStartupCost = 0.0;
	}

	/* IO cost: read posting pages. */
	indexCost = numIndexPages * random_page_cost;

	/* CPU cost: AND/OR merge of sparsemaps. */
	indexCost += numIndexTuples * cpu_operator_cost;

	/* Recheck cost: TRE regex matching is expensive.
	 * k=0 (exact match): 10x cpu_operator_cost
	 * k>0 (approximate): 20x cpu_operator_cost (universal-Levenshtein) */
	if (have_query && q.global_max_cost > 0)
	{
		indexCost += numIndexTuples * cpu_operator_cost * 20.0;
	}
	else
	{
		indexCost += numIndexTuples * cpu_operator_cost * 10.0;
	}

	*indexTotalCost = *indexStartupCost + indexCost;
	*indexSelectivity = sel;
	*indexCorrelation = 0.0;
	*indexPages = numIndexPages;
}
