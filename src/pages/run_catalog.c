/*
 * src/pages/run_catalog.c - run/level catalog iteration (Phase B1,
 * format v7).
 *
 * This increment implements only the single-implicit-run case (the
 * default for v6 indexes and freshly built v7 indexes) plus the
 * scaffold to walk a real catalog page chain once later increments
 * populate one.  With one run the iterator yields exactly the meta
 * page's root_upper/root_range, so the scan path is identical to
 * pre-v7.
 */

#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"
#include "access/relation.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/relcache.h"

#include "pg_tre/buffer.h"
#include "pg_tre/meta.h"
#include "pg_tre/page.h"
#include "pg_tre/run_catalog.h"


struct PgTreRunIter
{
    Relation    index;

    /* Implicit base run (run_id 0), served first. */
    bool        implicit_served;
    PgTreRun    implicit_run;

    /* Catalog-chain walk for additional runs (run_id >= 1). */
    BlockNumber cur_page;       /* current catalog page, or Invalid */
    int         cur_idx;        /* index into the current page's runs */
    int         cur_n;          /* run count on the current page */
};

PgTreRunIter *
pg_tre_run_catalog_open(Relation index)
{
    PgTreRunIter      *it = (PgTreRunIter *) palloc0(sizeof(*it));
    PgTreMetaPageData  meta;

    pg_tre_meta_read(index, &meta);
    it->index = index;

    /*
     * The implicit base run (run_id 0) is ALWAYS present: it is the
     * structure rooted at the meta page's root_upper/root_range and
     * holds the bulk of the index's data.  Catalog runs (run_id >= 1)
     * are ADDITIONAL runs flushed on top of it; they are never a
     * replacement for the base run until a real merge consumes it.
     * The iterator therefore yields the implicit base run first
     * (full trigram range -- never skipped), then walks the catalog
     * chain for any additional runs.
     */
    it->implicit_served = false;
    it->implicit_run.run_id = 0;
    it->implicit_run.level = 1;
    it->implicit_run.flags = PG_TRE_RUN_LIVE;
    it->implicit_run.root_upper = meta.root_upper;
    it->implicit_run.root_range = meta.root_range;
    it->implicit_run._pad0 = 0;
    it->implicit_run.n_tuples = meta.n_tuples_indexed;
    it->implicit_run.n_trigrams = meta.n_trigrams;
    it->implicit_run.min_trigram_hash = 0;
    it->implicit_run.max_trigram_hash = UINT64_MAX;

    if (meta.run_catalog_head == InvalidBlockNumber)
    {
        /* No catalog runs: only the implicit base run exists. */
        it->cur_page = InvalidBlockNumber;
    }
    else
    {
        it->cur_page = meta.run_catalog_head;
    }
    it->cur_idx = 0;
    it->cur_n = 0;
    return it;
}

/*
 * Load the current catalog page's run count into the iterator,
 * advancing past empty pages.  Returns false when the chain ends.
 */
static bool
catalog_load_page(PgTreRunIter *it)
{
    while (BlockNumberIsValid(it->cur_page))
    {
        Buffer  buf = pg_tre_read(it->index, it->cur_page,
                                  PG_TRE_PAGE_RUN_CATALOG,
                                  BUFFER_LOCK_SHARE);
        Page    page = BufferGetPage(buf);
        PgTreRunCatalogHeader *hdr =
            (PgTreRunCatalogHeader *) PageGetContents(page);

        it->cur_n = (int) hdr->n_entries;
        it->cur_idx = 0;
        if (it->cur_n > 0)
        {
            UnlockReleaseBuffer(buf);
            return true;
        }
        /* empty page: follow the chain */
        it->cur_page = hdr->right_link;
        UnlockReleaseBuffer(buf);
    }
    return false;
}

bool
pg_tre_run_catalog_next(PgTreRunIter *it, PgTreRun *out)
{
    /* Serve the implicit base run (run_id 0) first. */
    if (!it->implicit_served)
    {
        it->implicit_served = true;
        *out = it->implicit_run;
        return true;
    }

    /* No catalog chain: only the implicit run exists. */
    if (!BlockNumberIsValid(it->cur_page) && it->cur_n == 0)
        return false;

    /* Then walk catalog runs (run_id >= 1). */
    for (;;)
    {
        CHECK_FOR_INTERRUPTS();

        if (it->cur_idx >= it->cur_n)
        {
            /* need the next page (or first page) */
            if (it->cur_n > 0)
            {
                /* advance cur_page to right_link of the page we just
                 * finished; re-read it to follow the chain */
                Buffer  buf = pg_tre_read(it->index, it->cur_page,
                                          PG_TRE_PAGE_RUN_CATALOG,
                                          BUFFER_LOCK_SHARE);
                Page    page = BufferGetPage(buf);
                PgTreRunCatalogHeader *hdr =
                    (PgTreRunCatalogHeader *) PageGetContents(page);
                BlockNumber next = hdr->right_link;
                UnlockReleaseBuffer(buf);
                it->cur_page = next;
            }
            if (!catalog_load_page(it))
                return false;
        }

        {
            Buffer  buf = pg_tre_read(it->index, it->cur_page,
                                      PG_TRE_PAGE_RUN_CATALOG,
                                      BUFFER_LOCK_SHARE);
            Page    page = BufferGetPage(buf);
            PgTreRun *runs = (PgTreRun *)
                (((char *) PageGetContents(page)) +
                 MAXALIGN(sizeof(PgTreRunCatalogHeader)));
            PgTreRun r = runs[it->cur_idx];
            UnlockReleaseBuffer(buf);
            it->cur_idx++;

            if (r.flags & PG_TRE_RUN_LIVE)
            {
                *out = r;
                return true;
            }
            /* skip non-live runs */
        }
    }
}

void
pg_tre_run_catalog_close(PgTreRunIter *it)
{
    if (it != NULL)
        pfree(it);
}

uint32
pg_tre_run_count(Relation index)
{
    PgTreMetaPageData meta;

    pg_tre_meta_read(index, &meta);
    /* n_runs already counts the implicit base run (it is set to 2 on
     * the first catalog append: implicit + new).  Zero means a fresh
     * index with only the implicit run. */
    if (meta.n_runs == 0)
        return 1;
    return meta.n_runs;
}


/*
 * tre_run_catalog_status(idx regclass)
 *   RETURNS TABLE(run_id bigint, level int, n_tuples bigint,
 *                 n_trigrams bigint, min_hash numeric, max_hash numeric)
 *
 * Introspection over the run/level catalog (Phase B1).  For a v6 or
 * default-v7 index this returns the single implicit run.  Exercises
 * the run iterator end-to-end.
 */
PG_FUNCTION_INFO_V1(tre_run_catalog_status);
Datum
tre_run_catalog_status(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    ReturnSetInfo      *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    Relation            index;
    PgTreRunIter       *it;
    PgTreRun            run;
    TupleDesc           tupdesc;
    Tuplestorestate    *tupstore;
    MemoryContext       per_query_ctx;
    MemoryContext       oldcontext;

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that "
                        "cannot accept a set")));

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");

    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    oldcontext = MemoryContextSwitchTo(per_query_ctx);
    tupstore = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;
    MemoryContextSwitchTo(oldcontext);

    index = relation_open(relid, AccessShareLock);

    it = pg_tre_run_catalog_open(index);
    while (pg_tre_run_catalog_next(it, &run))
    {
        Datum   values[6];
        bool    nulls[6] = {false, false, false, false, false, false};
        char    buf[32];

        values[0] = Int64GetDatum((int64) run.run_id);
        values[1] = Int32GetDatum((int32) run.level);
        values[2] = Int64GetDatum((int64) run.n_tuples);
        values[3] = Int64GetDatum((int64) run.n_trigrams);
        snprintf(buf, sizeof(buf), UINT64_FORMAT, run.min_trigram_hash);
        values[4] = DirectFunctionCall3(numeric_in, CStringGetDatum(buf),
                                        ObjectIdGetDatum(InvalidOid),
                                        Int32GetDatum(-1));
        snprintf(buf, sizeof(buf), UINT64_FORMAT, run.max_trigram_hash);
        values[5] = DirectFunctionCall3(numeric_in, CStringGetDatum(buf),
                                        ObjectIdGetDatum(InvalidOid),
                                        Int32GetDatum(-1));
        tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
    pg_tre_run_catalog_close(it);

    relation_close(index, AccessShareLock);
    return (Datum) 0;
}

