/*
 * src/am/sel.c - restriction selectivity for %~~ operator.
 *
 * Phase 6: tre_pattern_sel() estimates what fraction of rows will match
 * a regex pattern, allowing the planner to choose between index scan and
 * seq scan based on real cost estimates.
 */

#include "postgres.h"

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
#include "pg_tre/regex_ast.h"

#define DEFAULT_REGEX_SELECTIVITY 0.01

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
	index_close(index, AccessShareLock);

	sel = estimate_trigram_selectivity(&q, &meta);

	ReleaseVariableStats(vardata);
	PG_RETURN_FLOAT8((float8) sel);
}
