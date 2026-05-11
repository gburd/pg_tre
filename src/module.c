/*
 * src/module.c - pg_tre module initialization.
 *
 * PG_MODULE_MAGIC, _PG_init, GUC registration, rmgr registration, and
 * the legacy tre_amatch* UDFs inherited from the 0.1.0 UDF-only extension.
 * The index AM callbacks live under src/am/.
 */

#include "postgres.h"

#include <limits.h>

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "pg_tre/pg_tre.h"
#include "pg_tre/tre_match.h"
#include "pg_tre/pattern_cache.h"
#include "pg_tre/xlog.h"

PG_MODULE_MAGIC;

/* ---- GUCs ---- */

int  pg_tre_default_max_cost       = 3;
int  pg_tre_pending_list_limit_kb  = 4096;   /* 4 MiB */
int  pg_tre_range_size_blocks      = 128;
int  pg_tre_bloom_tuple_bits       = 128;
int  pg_tre_max_extraction_fanout  = 256;
int  pg_tre_max_nfa_states         = 10000;
int  pg_tre_compile_timeout_ms     = 1000;
int  pg_tre_match_timeout_ms       = 1000;
bool pg_tre_fastupdate             = true;
bool pg_tre_tuple_bloom_enable     = true;

void
pg_tre_init_guc(void)
{
    DefineCustomIntVariable("pg_tre.default_max_cost",
        "Default maximum approximate-match edit cost when unspecified.",
        NULL,
        &pg_tre_default_max_cost,
        3, 0, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.pending_list_limit",
        "Maximum size of the fast-update pending list, in KiB.",
        NULL,
        &pg_tre_pending_list_limit_kb,
        4096, 64, INT_MAX, PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.range_size_blocks",
        "Heap blocks summarized by each range-bloom entry.",
        NULL,
        &pg_tre_range_size_blocks,
        128, 1, 131072, PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.bloom_tuple_bits",
        "Bits per per-tuple bloom filter.",
        NULL,
        &pg_tre_bloom_tuple_bits,
        128, 32, 1024, PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.max_extraction_fanout",
        "Maximum number of trigram disjuncts a query may emit.",
        NULL,
        &pg_tre_max_extraction_fanout,
        256, 1, 65536, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.max_nfa_states",
        "Reject patterns whose compiled NFA exceeds this state count.",
        NULL,
        &pg_tre_max_nfa_states,
        10000, 32, 1000000, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.compile_timeout_ms",
        "Maximum regex-compile time before aborting (milliseconds).",
        NULL,
        &pg_tre_compile_timeout_ms,
        1000, 1, 600000, PGC_USERSET, GUC_UNIT_MS, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.match_timeout_ms",
        "Per-query match-time budget (milliseconds).",
        NULL,
        &pg_tre_match_timeout_ms,
        1000, 1, 600000, PGC_USERSET, GUC_UNIT_MS, NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_tre.fastupdate",
        "Enable the fast-update pending list for inserts.",
        NULL,
        &pg_tre_fastupdate,
        true, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_tre.tuple_bloom_enable",
        "Enable per-tuple bloom filters in posting leaves (Phase 5).",
        NULL,
        &pg_tre_tuple_bloom_enable,
        true, PGC_SIGHUP, 0, NULL, NULL, NULL);

    MarkGUCPrefixReserved("pg_tre");
}

/* ---- rmgr registration ---- */

static RmgrData pg_tre_rmgr = {
    .rm_name          = RM_PG_TRE_NAME,
    .rm_redo          = pg_tre_redo,
    .rm_desc          = pg_tre_desc,
    .rm_identify      = pg_tre_identify,
    .rm_startup       = pg_tre_startup,
    .rm_cleanup       = pg_tre_cleanup,
    .rm_mask          = pg_tre_mask,
    .rm_decode        = NULL,
};

void
pg_tre_init_rmgr(void)
{
    RegisterCustomRmgr(RM_PG_TRE_ID, &pg_tre_rmgr);
}

/* ---- _PG_init ---- */

void _PG_init(void);

void
_PG_init(void)
{
    pg_tre_init_guc();

    /*
     * Custom resource managers must be registered during preload.
     * When pg_tre is loaded on demand (CREATE EXTENSION without
     * shared_preload_libraries), skip the rmgr registration: the
     * legacy UDFs still work, but the AM's write path will reject
     * index mutations until a preload-enabled restart.
     */
    if (process_shared_preload_libraries_in_progress)
        pg_tre_init_rmgr();

    tre_cache_init();
}

/* ====================================================================
 * Legacy UDFs preserved from pg_tre 0.1.0.  These keep the UDF-only
 * interface working during the AM transition and will be retained in
 * 1.0.0 as convenience functions.
 * ==================================================================== */

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
    PG_RETURN_TEXT_P(cstring_to_text(PG_TRE_VERSION_STRING " (TRE 0.9.0)"));
}
