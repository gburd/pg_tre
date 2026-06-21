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
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "common/string.h"

#include "pg_tre/buffer.h"
#include "pg_tre/meta.h"
#include "pg_tre/page.h"
#include "pg_tre/run_catalog.h"
#include "pg_tre/xlog.h"

/* Max PgTreRun records that fit on one catalog page after the header.
 * Defined in run_catalog.h (also used by pending.c's Hanoi merge). */


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

/*
 * Append a new run to the catalog (Phase B1.3).
 *
 * Allocates run->run_id from the meta page's monotonic next_run_id,
 * stamps it into *run, appends the record to the head catalog page
 * (creating the first catalog page on demand), and bumps n_runs.
 *
 * Crash-safety (the bug that blocked B1.2, now fixed): all page
 * modifications -- the catalog-page append AND the meta-page update --
 * happen inside ONE critical section, and both buffers are WAL-logged
 * as full-page images under XLOG_PTRE_META_UPDATE in a single record.
 * New catalog pages use REGBUF_FORCE_IMAGE (NOT REGBUF_WILL_INIT):
 * pg_tre_extend has already physically extended the relation, so the
 * block exists at replay and the generic pg_tre_redo_fpi restores the
 * image without the WILL_INIT zeroing-redo contract (which our FPI
 * redo does not implement and which PANICs recovery if violated).
 *
 * Caller must hold a lock excluding concurrent catalog writers (the
 * flush/merge path holds the index's ShareUpdateExclusive).  The
 * implicit run (run_id 0) is not stored; the first append yields a
 * 2-run index (implicit + new).  Returns the assigned run_id.
 */
uint64
pg_tre_run_catalog_append(Relation index, PgTreRun *run)
{
    Buffer      metabuf;
    Page        metapage;
    PgTreMetaPage meta;
    Buffer      catbuf;
    Page        catpage;
    PgTreRunCatalogHeader *hdr;
    PgTreRun   *runs;
    uint64      assigned_id;

    metabuf = pg_tre_read(index, PG_TRE_META_BLKNO, PG_TRE_PAGE_META,
                          BUFFER_LOCK_EXCLUSIVE);
    metapage = BufferGetPage(metabuf);
    meta = PgTreMetaPageGet(metapage);

    /* Normalize a pre-v7 meta tail in place (matches pg_tre_meta_read). */
    if (meta->next_run_id == 0)
        meta->next_run_id = 1;
    if (meta->max_levels == 0)
        meta->max_levels = 7;
    if (meta->n_runs == 0 && meta->run_catalog_head == 0)
        meta->run_catalog_head = InvalidBlockNumber;

    assigned_id = meta->next_run_id;
    run->run_id = assigned_id;

    /* Obtain a catalog page with room, or create the first/new head. */
    if (meta->run_catalog_head == InvalidBlockNumber)
    {
        catbuf = pg_tre_extend(index, PG_TRE_PAGE_RUN_CATALOG);
        catpage = BufferGetPage(catbuf);
        hdr = (PgTreRunCatalogHeader *) PageGetContents(catpage);
        hdr->right_link = InvalidBlockNumber;
        hdr->n_entries = 0;
        hdr->_pad0 = 0;
    }
    else
    {
        catbuf = pg_tre_read(index, meta->run_catalog_head,
                             PG_TRE_PAGE_RUN_CATALOG, BUFFER_LOCK_EXCLUSIVE);
        catpage = BufferGetPage(catbuf);
        hdr = (PgTreRunCatalogHeader *) PageGetContents(catpage);

        if (hdr->n_entries >= RUN_CATALOG_CAP)
        {
            /* Head full: chain a fresh head page in front (newest-first). */
            BlockNumber old_head = meta->run_catalog_head;
            UnlockReleaseBuffer(catbuf);
            catbuf = pg_tre_extend(index, PG_TRE_PAGE_RUN_CATALOG);
            catpage = BufferGetPage(catbuf);
            hdr = (PgTreRunCatalogHeader *) PageGetContents(catpage);
            hdr->right_link = old_head;
            hdr->n_entries = 0;
            hdr->_pad0 = 0;
        }
    }

    runs = (PgTreRun *)
        (((char *) PageGetContents(catpage)) +
         MAXALIGN(sizeof(PgTreRunCatalogHeader)));

    /* All page edits + WAL inside one critical section. */
    START_CRIT_SECTION();

    runs[hdr->n_entries] = *run;
    hdr->n_entries++;
    ((PageHeader) catpage)->pd_lower =
        ((char *) &runs[hdr->n_entries] - (char *) catpage);

    meta->next_run_id = assigned_id + 1;
    meta->run_catalog_head = BufferGetBlockNumber(catbuf);
    if (meta->n_runs == 0)
        meta->n_runs = 2;   /* implicit base run + this one */
    else
        meta->n_runs++;

    MarkBufferDirty(catbuf);
    MarkBufferDirty(metabuf);

    if (RelationNeedsWAL(index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, metabuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        XLogRegisterBuffer(1, catbuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_META_UPDATE);
        PageSetLSN(catpage, recptr);
        PageSetLSN(metapage, recptr);
    }

    END_CRIT_SECTION();

    UnlockReleaseBuffer(catbuf);
    UnlockReleaseBuffer(metabuf);
    return assigned_id;
}

/*
 * Collapse the run catalog back to a single implicit run (Phase B1.4
 * adaptive collapse).  The caller has merged all live runs' postings
 * into `new_root_upper` (a fresh upper tree); this atomically swaps
 * the meta page's root_upper to it and resets the catalog to the
 * single-implicit-run state (run_catalog_head = Invalid, n_runs = 0),
 * so the index converges to the pre-v7 single-structure layout and
 * scan cost returns to baseline.  The old catalog pages and the old
 * base/run upper trees are leaked to the FSM's later reclamation
 * (same deferred-recycle policy as the merge path); they are no
 * longer reachable.  WAL-logged as a single meta-page FPI.
 *
 * Caller must hold a lock excluding concurrent catalog writers.
 */
void
pg_tre_run_catalog_collapse_reset(Relation index, BlockNumber new_root_upper,
                                  BlockNumber new_root_range,
                                  uint64 n_trigrams, uint64 n_tuples)
{
    Buffer        metabuf;
    Page          metapage;
    PgTreMetaPage meta;

    metabuf = pg_tre_read(index, PG_TRE_META_BLKNO, PG_TRE_PAGE_META,
                          BUFFER_LOCK_EXCLUSIVE);
    metapage = BufferGetPage(metabuf);
    meta = PgTreMetaPageGet(metapage);

    START_CRIT_SECTION();

    meta->root_upper       = new_root_upper;
    meta->root_range       = new_root_range;
    meta->n_trigrams       = n_trigrams;
    meta->n_tuples_indexed = n_tuples;
    /* Reset to single-implicit-run.  Keep next_run_id monotonic. */
    meta->run_catalog_head = InvalidBlockNumber;
    meta->n_runs           = 0;

    MarkBufferDirty(metabuf);

    if (RelationNeedsWAL(index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, metabuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_META_UPDATE);
        PageSetLSN(metapage, recptr);
    }

    END_CRIT_SECTION();

    UnlockReleaseBuffer(metabuf);
}

/*
 * Replace the ENTIRE run catalog with the given list of runs
 * (Phase B1.4 Hanoi incremental merge).  Writes the runs onto a
 * single fresh catalog page and points the meta page at it, all in
 * one WAL'd critical section.  The base implicit run (run_id 0) is
 * NOT part of `runs` and is never touched -- only catalog runs
 * (run_id >= 1) are rewritten.  `next_run_id` is left untouched
 * (monotonic).  n_runs becomes 1 (implicit) + n.  Old catalog pages
 * and the upper trees of dropped runs are leaked to FSM reclamation
 * (same deferred-recycle policy as the merge path).
 *
 * Used by the level-merge driver: read the live catalog runs, merge
 * the over-capacity level into one promoted run, then call this with
 * (survivors + promoted) to install the new catalog atomically.
 *
 * If `n` is 0, resets to the single implicit run (run_catalog_head =
 * Invalid).  Caller holds a lock excluding concurrent catalog writers.
 */
void
pg_tre_run_catalog_rewrite(Relation index, const PgTreRun *runs, int n)
{
    Buffer        metabuf;
    Page          metapage;
    PgTreMetaPage meta;
    Buffer        catbuf = InvalidBuffer;
    Page          catpage = NULL;
    BlockNumber   cat_blk = InvalidBlockNumber;

    if (n < 0)
        n = 0;
    if (n > (int) RUN_CATALOG_CAP)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("pg_tre: run catalog rewrite exceeds one page (%d > %d)",
                        n, (int) RUN_CATALOG_CAP)));

    metabuf = pg_tre_read(index, PG_TRE_META_BLKNO, PG_TRE_PAGE_META,
                          BUFFER_LOCK_EXCLUSIVE);
    metapage = BufferGetPage(metabuf);
    meta = PgTreMetaPageGet(metapage);

    if (n > 0)
    {
        PgTreRunCatalogHeader *hdr;
        PgTreRun *dst;
        int i;

        catbuf = pg_tre_extend(index, PG_TRE_PAGE_RUN_CATALOG);
        catpage = BufferGetPage(catbuf);
        cat_blk = BufferGetBlockNumber(catbuf);
        hdr = (PgTreRunCatalogHeader *) PageGetContents(catpage);
        hdr->right_link = InvalidBlockNumber;
        hdr->n_entries = (uint32) n;
        hdr->_pad0 = 0;
        dst = (PgTreRun *)
            (((char *) PageGetContents(catpage)) +
             MAXALIGN(sizeof(PgTreRunCatalogHeader)));
        for (i = 0; i < n; i++)
            dst[i] = runs[i];
        ((PageHeader) catpage)->pd_lower =
            ((char *) &dst[n] - (char *) catpage);
    }

    START_CRIT_SECTION();

    meta->run_catalog_head = (n > 0) ? cat_blk : InvalidBlockNumber;
    meta->n_runs           = (n > 0) ? (uint32) (n + 1) : 0;

    if (n > 0)
        MarkBufferDirty(catbuf);
    MarkBufferDirty(metabuf);

    if (RelationNeedsWAL(index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, metabuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        if (n > 0)
            XLogRegisterBuffer(1, catbuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_META_UPDATE);
        PageSetLSN(metapage, recptr);
        if (n > 0)
            PageSetLSN(catpage, recptr);
    }

    END_CRIT_SECTION();

    if (BufferIsValid(catbuf))
        UnlockReleaseBuffer(catbuf);
    UnlockReleaseBuffer(metabuf);
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


/*
 * tre_debug_append_run(idx regclass, min_hash numeric, max_hash numeric)
 *   RETURNS bigint
 *
 * TEST-ONLY (Phase B1.3): register the index's current roots as an
 * additional run with the given trigram-hash range, exercising the
 * crash-safe catalog writer and the multi-run scan path before the
 * production flush-to-run lands.  Shares the existing run's roots, so
 * it validates the catalog + WAL + scan plumbing, not real run
 * isolation.  Not part of the supported API; used by
 * tap/crash_recovery.pl to prove catalog appends survive kill -9.
 */
PG_FUNCTION_INFO_V1(tre_debug_append_run);
Datum
tre_debug_append_run(PG_FUNCTION_ARGS)
{
    Oid                relid = PG_GETARG_OID(0);
    Numeric            min_num = PG_GETARG_NUMERIC(1);
    Numeric            max_num = PG_GETARG_NUMERIC(2);
    uint64             min_hash;
    uint64             max_hash;
    Relation           index;
    PgTreMetaPageData  meta;
    PgTreRun           run;
    uint64             id;
    char              *s;

    /* Trigram hashes span the full uint64 range; parse via text so
     * values above 2^63 round-trip (numeric_int8 would overflow). */
    s = DatumGetCString(DirectFunctionCall1(numeric_out,
                                            NumericGetDatum(min_num)));
    min_hash = strtou64(s, NULL, 10);
    pfree(s);
    s = DatumGetCString(DirectFunctionCall1(numeric_out,
                                            NumericGetDatum(max_num)));
    max_hash = strtou64(s, NULL, 10);
    pfree(s);

    index = relation_open(relid, RowExclusiveLock);
    pg_tre_meta_read(index, &meta);

    memset(&run, 0, sizeof(run));
    run.level = 1;
    run.flags = PG_TRE_RUN_LIVE;
    run.root_upper = meta.root_upper;
    run.root_range = meta.root_range;
    run.n_tuples = meta.n_tuples_indexed;
    run.n_trigrams = meta.n_trigrams;
    run.min_trigram_hash = min_hash;
    run.max_trigram_hash = max_hash;

    id = pg_tre_run_catalog_append(index, &run);

    relation_close(index, RowExclusiveLock);
    PG_RETURN_INT64((int64) id);
}


/*
 * tre_coalesced_page_count(idx regclass) RETURNS bigint
 *
 * Diagnostic: count PG_TRE_PAGE_POSTING_COALESCED pages in the index.
 * Used by the coalesce regression to confirm coalescing actually
 * engaged (a coalesced page exists) so the VACUUM/merge safety paths
 * are genuinely exercised, and by operators deciding whether to enable
 * pg_tre.coalesce_enable.  Walks every block under AccessShareLock.
 */
PG_FUNCTION_INFO_V1(tre_coalesced_page_count);
Datum
tre_coalesced_page_count(PG_FUNCTION_ARGS)
{
    Oid          relid = PG_GETARG_OID(0);
    Relation     index;
    BlockNumber  nblocks;
    BlockNumber  blk;
    int64        count = 0;

    index = relation_open(relid, AccessShareLock);
    nblocks = RelationGetNumberOfBlocks(index);

    for (blk = 0; blk < nblocks; blk++)
    {
        Buffer  buf = ReadBuffer(index, blk);
        Page    page;

        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);
        if (!PageIsNew(page) &&
            PageTreGetOpaque(page)->page_kind == PG_TRE_PAGE_POSTING_COALESCED)
            count++;
        UnlockReleaseBuffer(buf);
    }

    relation_close(index, AccessShareLock);
    PG_RETURN_INT64(count);
}


/*
 * tre_page_kind_histogram(idx regclass)
 *   RETURNS TABLE(page_kind text, n_pages bigint,
 *                 used_bytes bigint, used_pct numeric)
 *
 * Diagnostic: walk every block of the index and tally page counts by
 * kind, plus total used bytes per kind (pd_lower, i.e. header + written
 * content).  Read-only, AccessShareLock, no format dependency.  Tells an
 * operator (or a regression) WHICH page kind dominates an index's size
 * and whether those pages are full or mostly empty -- the fast way to
 * localize a density blowup to a specific allocator.  "new/empty" counts
 * pages the relation was extended to but never initialized.
 */
PG_FUNCTION_INFO_V1(tre_page_kind_histogram);
Datum
tre_page_kind_histogram(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    ReturnSetInfo      *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    Relation            index;
    BlockNumber         nblocks;
    BlockNumber         blk;
    TupleDesc           tupdesc;
    Tuplestorestate    *tupstore;
    MemoryContext       per_query_ctx;
    MemoryContext       oldcontext;

    /* kind ids 0..10 plus a "new/empty" bucket at index 11. */
#define PKH_NKINDS 12
    int64   pages[PKH_NKINDS];
    int64   used[PKH_NKINDS];
    int     i;
    static const char *kind_name[PKH_NKINDS] = {
        "invalid", "meta", "upper", "upper_leaf", "posting",
        "posting_leaf", "range", "pending", "run_catalog",
        "posting_coalesced", "free_log", "new_or_empty"
    };

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

    for (i = 0; i < PKH_NKINDS; i++)
    {
        pages[i] = 0;
        used[i] = 0;
    }

    index = relation_open(relid, AccessShareLock);
    nblocks = RelationGetNumberOfBlocks(index);

    for (blk = 0; blk < nblocks; blk++)
    {
        Buffer  buf = ReadBuffer(index, blk);
        Page    page;
        int     slot;

        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);

        if (PageIsNew(page))
        {
            slot = PKH_NKINDS - 1;   /* new_or_empty */
        }
        else
        {
            uint16 kind = PageTreGetOpaque(page)->page_kind;
            slot = (kind < PKH_NKINDS - 1) ? (int) kind : 0;
            used[slot] += (int64) ((PageHeader) page)->pd_lower;
        }
        pages[slot]++;
        UnlockReleaseBuffer(buf);
    }

    relation_close(index, AccessShareLock);

    for (i = 0; i < PKH_NKINDS; i++)
    {
        Datum   values[4];
        bool    nulls[4] = {false, false, false, false};
        char    buf[32];
        int64   cap = pages[i] * (int64) BLCKSZ;

        if (pages[i] == 0)
            continue;

        values[0] = CStringGetTextDatum(kind_name[i]);
        values[1] = Int64GetDatum(pages[i]);
        values[2] = Int64GetDatum(used[i]);
        snprintf(buf, sizeof(buf), "%.1f",
                 cap > 0 ? (100.0 * (double) used[i] / (double) cap) : 0.0);
        values[3] = DirectFunctionCall3(numeric_in, CStringGetDatum(buf),
                                        ObjectIdGetDatum(InvalidOid),
                                        Int32GetDatum(-1));
        tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
#undef PKH_NKINDS

    return (Datum) 0;
}
