/*
 * src/util/upgrade.c - in-place format upgrade for pg_tre indexes.
 *
 * Implements the C entry points for
 *
 *    pg_tre_upgrade_index(idx regclass) RETURNS void
 *    pg_tre_index_format_status(idx regclass)
 *        RETURNS TABLE(format_version int4, page_count bigint)
 *    pg_tre_index_min_format_version(idx regclass) RETURNS int4
 *
 * declared in sql/pg_tre--1.4.0-dev.sql.
 *
 * Design (see doc/onpage_format.md):
 *
 *   - PageTreOpaqueData->format_version is the *per-page* on-disk
 *     format version.  Readers in [MIN, LATEST] are accepted; writers
 *     always emit LATEST.
 *
 *   - PgTreMetaPageData->min_page_format_version is the minimum
 *     per-page format_version observed across all pages.  When equal
 *     to LATEST, the index is fully upgraded.
 *
 *   - pg_tre_upgrade_index() walks every block in EXCLUSIVE mode (one
 *     at a time, brief lock per page), rewrites pages whose format_version
 *     is below LATEST, WAL-logs each rewrite as XLOG_PTRE_PAGE_FORMAT_UPGRADE
 *     (FORCE_IMAGE / one record per page), and at the end of the sweep
 *     bumps min_page_format_version on the meta page if every page is now
 *     at LATEST.
 *
 *   - "Rewriting a page" today is a no-op because v3 and v4 are byte-
 *     identical; setting opq->format_version = LATEST is the only
 *     change.  The follow-on commit (variable-width per-tuple blooms)
 *     will fill in the real conversion logic in
 *     pg_tre_upgrade_page_to_latest().
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "commands/defrem.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lockdefs.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "pg_tre/buffer.h"
#include "pg_tre/meta.h"
#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/upgrade.h"
#include "pg_tre/xlog.h"

/*
 * Verify that 'rel' is a pg_tre index.  Errors out otherwise.
 *
 * Caller has already opened the relation; we test the AM by name to
 * avoid dragging in a build-time dependency on the AM oid (it varies
 * per-cluster) and to give a clear diagnostic.
 */
static void
check_is_pg_tre_index(Relation rel)
{
    if (rel->rd_rel->relkind != RELKIND_INDEX &&
        rel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("\"%s\" is not an index",
                        RelationGetRelationName(rel))));

    if (rel->rd_rel->relam == InvalidOid)
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("\"%s\" is not a pg_tre index",
                        RelationGetRelationName(rel))));

    {
        char *amname = get_am_name(rel->rd_rel->relam);

        if (amname == NULL || strcmp(amname, "tre") != 0)
            ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                     errmsg("\"%s\" is not a pg_tre index "
                            "(uses access method \"%s\")",
                            RelationGetRelationName(rel),
                            amname ? amname : "?")));
        if (amname != NULL)
            pfree(amname);
    }
}

/*
 * Convert a single page's bytes from any supported older format to LATEST,
 * in place.  Caller holds the buffer's exclusive lock.
 *
 * Returns true if the page bytes were modified (caller should mark
 * dirty + WAL-log), false if no changes were needed (already at LATEST,
 * or page is new/empty).
 *
 * Today this only updates opq->format_version, because v3 and v4 share
 * a byte layout.  When a future format introduces a real rewrite (e.g.
 * variable-width per-tuple blooms) the per-version arms go here.
 */
static bool
pg_tre_upgrade_page_to_latest(Page page)
{
    PageTreOpaque opq;
    uint32 from;

    if (PageIsNew(page))
        return false;

    opq = PageTreGetOpaque(page);
    from = opq->format_version;

    if (from == PG_TRE_FORMAT_VERSION_LATEST)
        return false;

    if (from < PG_TRE_FORMAT_VERSION_MIN || from > PG_TRE_FORMAT_VERSION_LATEST)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_tre: unsupported page format version %u "
                        "(supported range [%u, %u])",
                        from,
                        (uint32) PG_TRE_FORMAT_VERSION_MIN,
                        (uint32) PG_TRE_FORMAT_VERSION_LATEST)));

    /*
     * Per-version dispatch goes here.  v3 -> v4 is byte-identical for
     * every page kind.  v4 -> v5 is byte-identical for every page kind
     * EXCEPT PG_TRE_PAGE_RANGE, which gained a PgTreRangeHeader at the
     * start of its content area.  We can't manufacture that header
     * from a legacy single-page range layout without rewriting offsets
     * (and pre-v5 range pages are guaranteed to be the only page in
     * the chain), so we leave them at their existing format and let
     * the read paths handle them via the v<5 back-compat branch.
     */
    if (opq->page_kind == (uint16) PG_TRE_PAGE_RANGE && from < 5)
        return false;

    switch (from)
    {
        case 3:
        case 4:
            /*
             * Non-range pages: byte-identical across v3, v4, v5.  We
             * just need to flip the per-page version stamp below.
             */
            break;
        default:
            elog(ERROR, "pg_tre: page format upgrade from v%u not implemented",
                 from);
    }

    opq->format_version = PG_TRE_FORMAT_VERSION_LATEST;
    return true;
}

/*
 * Update the meta page's min_page_format_version after a full sweep.
 * Caller passes the minimum version observed; we WAL-log a meta-page
 * FPI on change.
 */
static void
pg_tre_meta_set_min_format_version(Relation index, uint32 min_version)
{
    Buffer metabuf;
    Page metapage;
    PgTreMetaPage meta;
    bool changed;

    metabuf = pg_tre_read(index, PG_TRE_META_BLKNO, PG_TRE_PAGE_META,
                          BUFFER_LOCK_EXCLUSIVE);
    metapage = BufferGetPage(metabuf);
    meta = PgTreMetaPageGet(metapage);

    /*
     * Treat zero (legacy reserved[] slot in 1.3.x indexes) the same as
     * meta->format_version: that's what pg_tre_meta_read patches a zero
     * value to.  We only emit a WAL record if the field actually moves.
     */
    {
        uint32 old = meta->min_page_format_version;
        if (old == 0)
            old = meta->format_version;
        changed = (old != min_version);
    }

    if (changed)
    {
        meta->min_page_format_version = min_version;
        MarkBufferDirty(metabuf);

        if (RelationNeedsWAL(index))
        {
            XLogRecPtr recptr;

            XLogBeginInsert();
            XLogRegisterBuffer(0, metabuf,
                               REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
            recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_META_UPDATE);
            PageSetLSN(metapage, recptr);
        }
    }

    UnlockReleaseBuffer(metabuf);
}

/*
 * pg_tre_upgrade_index(idx regclass) RETURNS void
 *
 * Walk every block of the index, rewriting pages below the target
 * format version in place.  Each page is held under EXCLUSIVE lock
 * only for the duration of its own rewrite + WAL emit.
 */
PG_FUNCTION_INFO_V1(pg_tre_upgrade_index);

Datum
pg_tre_upgrade_index(PG_FUNCTION_ARGS)
{
    Oid indexoid = PG_GETARG_OID(0);
    Relation index;
    BlockNumber nblocks;
    BlockNumber blkno;
    uint32 observed_min = PG_TRE_FORMAT_VERSION_LATEST;

    /*
     * Hold ShareUpdateExclusiveLock to block concurrent CREATE INDEX
     * CONCURRENTLY / VACUUM / other upgrade attempts on the same index,
     * but allow concurrent reads and writes.  Per-page exclusive locks
     * inside the walk serialise against scan/insert.
     */
    index = index_open(indexoid, ShareUpdateExclusiveLock);
    check_is_pg_tre_index(index);

    nblocks = RelationGetNumberOfBlocks(index);

    for (blkno = 0; blkno < nblocks; blkno++)
    {
        Buffer buf;
        Page page;
        PageTreOpaque opq;
        bool changed;

        CHECK_FOR_INTERRUPTS();

        buf = ReadBuffer(index, blkno);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        page = BufferGetPage(buf);

        if (PageIsNew(page))
        {
            UnlockReleaseBuffer(buf);
            continue;
        }

        opq = PageTreGetOpaque(page);

        /*
         * Defensive: refuse to touch pages that do not look like pg_tre
         * pages at all.  pg_tre_read does the same range check; we
         * replicate it here because we bypass pg_tre_read to skip the
         * page-kind assertion (we walk every kind in one loop).
         */
        if (opq->format_version < PG_TRE_FORMAT_VERSION_MIN ||
            opq->format_version > PG_TRE_FORMAT_VERSION_LATEST)
        {
            UnlockReleaseBuffer(buf);
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("pg_tre: index %s page %u has format version %u, "
                            "supported range is [%u, %u]",
                            RelationGetRelationName(index), blkno,
                            opq->format_version,
                            (uint32) PG_TRE_FORMAT_VERSION_MIN,
                            (uint32) PG_TRE_FORMAT_VERSION_LATEST)));
        }

        START_CRIT_SECTION();

        changed = pg_tre_upgrade_page_to_latest(page);

        if (changed)
        {
            MarkBufferDirty(buf);

            if (RelationNeedsWAL(index))
            {
                XLogRecPtr recptr;

                XLogBeginInsert();
                XLogRegisterBuffer(0, buf,
                                   REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
                recptr = XLogInsert(RM_PG_TRE_ID,
                                    XLOG_PTRE_PAGE_FORMAT_UPGRADE);
                PageSetLSN(page, recptr);
            }
        }

        END_CRIT_SECTION();

        /* Track the minimum across all pages we've seen so far. */
        if (opq->format_version < observed_min)
            observed_min = opq->format_version;

        UnlockReleaseBuffer(buf);
    }

    /*
     * Empty index (only the meta page would exist; meta is at block 0
     * and was already covered by the walk above): observed_min may
     * still be at its initial sentinel if nblocks == 0.  Default to
     * LATEST in that case so a freshly-built empty index reports
     * fully-upgraded.
     */
    if (nblocks == 0)
        observed_min = PG_TRE_FORMAT_VERSION_LATEST;

    pg_tre_meta_set_min_format_version(index, observed_min);

    index_close(index, ShareUpdateExclusiveLock);

    PG_RETURN_VOID();
}

/*
 * pg_tre_index_format_status(idx regclass)
 *     RETURNS TABLE(format_version int4, page_count bigint)
 *
 * Aggregate per-version page counts.  Walks all blocks under SHARED
 * locks; safe to run concurrently with reads and writes.
 */
PG_FUNCTION_INFO_V1(pg_tre_index_format_status);

Datum
pg_tre_index_format_status(PG_FUNCTION_ARGS)
{
    Oid indexoid = PG_GETARG_OID(0);
    Relation index;
    BlockNumber nblocks;
    BlockNumber blkno;
    /*
     * Counters indexed by version number.  We size for [0, LATEST+1)
     * which is generous: today LATEST == 4 so 8 slots is plenty.  If
     * a future LATEST grows past 31 this static cap needs to expand.
     */
    enum { MAX_VERSION_SLOT = 32 };
    uint64 counts[MAX_VERSION_SLOT];
    int v;

    ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc       tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext   per_query_ctx;
    MemoryContext   oldcontext;

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

    memset(counts, 0, sizeof(counts));

    index = index_open(indexoid, AccessShareLock);
    check_is_pg_tre_index(index);

    nblocks = RelationGetNumberOfBlocks(index);

    for (blkno = 0; blkno < nblocks; blkno++)
    {
        Buffer buf;
        Page page;
        PageTreOpaque opq;
        uint32 v_page;

        CHECK_FOR_INTERRUPTS();

        buf = ReadBuffer(index, blkno);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);

        if (PageIsNew(page))
        {
            UnlockReleaseBuffer(buf);
            continue;
        }

        opq = PageTreGetOpaque(page);
        v_page = opq->format_version;

        if (v_page < MAX_VERSION_SLOT)
            counts[v_page]++;
        else
        {
            UnlockReleaseBuffer(buf);
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("pg_tre: index %s page %u has out-of-range "
                            "format version %u",
                            RelationGetRelationName(index), blkno, v_page)));
        }

        UnlockReleaseBuffer(buf);
    }

    index_close(index, AccessShareLock);

    for (v = 0; v < MAX_VERSION_SLOT; v++)
    {
        Datum values[2];
        bool  nulls[2] = {false, false};

        if (counts[v] == 0)
            continue;
        values[0] = Int32GetDatum((int32) v);
        values[1] = Int64GetDatum((int64) counts[v]);
        tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }

    return (Datum) 0;
}

/*
 * pg_tre_index_min_format_version(idx regclass) RETURNS int4
 *
 * Returns the minimum per-page format_version across all pages of the
 * index, falling back to the meta page's tracked value when the index
 * is empty.  Cheap O(1) read (just the meta page); use
 * pg_tre_index_format_status for the authoritative full-walk view.
 */
PG_FUNCTION_INFO_V1(pg_tre_index_min_format_version);

Datum
pg_tre_index_min_format_version(PG_FUNCTION_ARGS)
{
    Oid indexoid = PG_GETARG_OID(0);
    Relation index;
    PgTreMetaPageData meta;

    index = index_open(indexoid, AccessShareLock);
    check_is_pg_tre_index(index);

    pg_tre_meta_read(index, &meta);

    index_close(index, AccessShareLock);

    PG_RETURN_INT32((int32) meta.min_page_format_version);
}
