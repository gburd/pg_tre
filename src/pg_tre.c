/*
 * pg_tre.c - PostgreSQL extension for approximate regex matching using TRE.
 *
 * Provides SQL-callable functions for fuzzy regex matching with
 * edit-distance-based cost control.  Compiled regex patterns are
 * cached per-session for performance.
 */

#include "postgres.h"

#include <limits.h>

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "tre_funcs.h"
#include "tre_cache.h"

PG_MODULE_MAGIC;

/* GUC: default maximum edit-distance cost */
static int pg_tre_default_max_cost = 3;

void _PG_init(void);

void
_PG_init(void)
{
    DefineCustomIntVariable("pg_tre.default_max_cost",
                            "Default maximum cost for approximate matching.",
                            NULL,
                            &pg_tre_default_max_cost,
                            3,          /* default */
                            0,          /* min */
                            INT_MAX,    /* max */
                            PGC_USERSET,
                            0,
                            NULL, NULL, NULL);

    tre_cache_init();
}

/* ---- SQL-callable functions ---- */

PG_FUNCTION_INFO_V1(pg_tre_amatch);

Datum
pg_tre_amatch(PG_FUNCTION_ARGS)
{
    text   *input    = PG_GETARG_TEXT_PP(0);
    text   *pattern  = PG_GETARG_TEXT_PP(1);
    int32   max_cost = PG_GETARG_INT32(2);
    void   *compiled;
    TreMatchResult result;

    compiled = tre_cache_lookup(VARDATA_ANY(pattern),
                                VARSIZE_ANY_EXHDR(pattern));

    result = tre_do_match(compiled,
                          VARDATA_ANY(input),
                          VARSIZE_ANY_EXHDR(input),
                          max_cost, 1, 1, 1,
                          INT_MAX, INT_MAX, INT_MAX, INT_MAX);

    PG_RETURN_BOOL(result.matched != 0);
}

PG_FUNCTION_INFO_V1(pg_tre_amatch_cost);

Datum
pg_tre_amatch_cost(PG_FUNCTION_ARGS)
{
    text   *input    = PG_GETARG_TEXT_PP(0);
    text   *pattern  = PG_GETARG_TEXT_PP(1);
    int32   max_cost = PG_GETARG_INT32(2);
    void   *compiled;
    TreMatchResult result;

    compiled = tre_cache_lookup(VARDATA_ANY(pattern),
                                VARSIZE_ANY_EXHDR(pattern));

    result = tre_do_match(compiled,
                          VARDATA_ANY(input),
                          VARSIZE_ANY_EXHDR(input),
                          max_cost, 1, 1, 1,
                          INT_MAX, INT_MAX, INT_MAX, INT_MAX);

    if (!result.matched)
        PG_RETURN_NULL();

    PG_RETURN_INT32(result.cost);
}

PG_FUNCTION_INFO_V1(pg_tre_amatch_with_costs);

Datum
pg_tre_amatch_with_costs(PG_FUNCTION_ARGS)
{
    text   *input      = PG_GETARG_TEXT_PP(0);
    text   *pattern    = PG_GETARG_TEXT_PP(1);
    int32   max_cost   = PG_GETARG_INT32(2);
    int32   cost_ins   = PG_GETARG_INT32(3);
    int32   cost_del   = PG_GETARG_INT32(4);
    int32   cost_subst = PG_GETARG_INT32(5);
    void   *compiled;
    TreMatchResult result;

    compiled = tre_cache_lookup(VARDATA_ANY(pattern),
                                VARSIZE_ANY_EXHDR(pattern));

    result = tre_do_match(compiled,
                          VARDATA_ANY(input),
                          VARSIZE_ANY_EXHDR(input),
                          max_cost, cost_ins, cost_del, cost_subst,
                          INT_MAX, INT_MAX, INT_MAX, INT_MAX);

    PG_RETURN_BOOL(result.matched != 0);
}

PG_FUNCTION_INFO_V1(pg_tre_amatch_detail);

Datum
pg_tre_amatch_detail(PG_FUNCTION_ARGS)
{
    ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc       tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext   per_query_ctx;
    MemoryContext   oldcontext;
    text   *input    = PG_GETARG_TEXT_PP(0);
    text   *pattern  = PG_GETARG_TEXT_PP(1);
    int32   max_cost = PG_GETARG_INT32(2);
    void   *compiled;
    TreMatchResult result;

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context "
                        "that cannot accept a set")));
    if (!(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required but not allowed "
                        "in this context")));

    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    oldcontext = MemoryContextSwitchTo(per_query_ctx);

    tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
    tupstore = tuplestore_begin_heap(false, false, work_mem);

    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;

    MemoryContextSwitchTo(oldcontext);

    compiled = tre_cache_lookup(VARDATA_ANY(pattern),
                                VARSIZE_ANY_EXHDR(pattern));

    result = tre_do_match(compiled,
                          VARDATA_ANY(input),
                          VARSIZE_ANY_EXHDR(input),
                          max_cost, 1, 1, 1,
                          INT_MAX, INT_MAX, INT_MAX, INT_MAX);

    if (result.matched)
    {
        Datum   values[6];
        bool    nulls[6];

        memset(nulls, 0, sizeof(nulls));
        values[0] = Int32GetDatum(result.cost);
        values[1] = Int32GetDatum(result.num_ins);
        values[2] = Int32GetDatum(result.num_del);
        values[3] = Int32GetDatum(result.num_subst);
        values[4] = Int32GetDatum(result.match_start);
        values[5] = Int32GetDatum(result.match_end);

        tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }

    return (Datum) 0;
}

PG_FUNCTION_INFO_V1(pg_tre_version);

Datum
pg_tre_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("pg_tre 0.1.0 (TRE 0.9.0)"));
}
