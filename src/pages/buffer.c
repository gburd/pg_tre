/*
 * src/pages/buffer.c - buffer-access helpers.
 */

#include "postgres.h"

#include "access/genam.h"
#include "miscadmin.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/lockdefs.h"
#include "storage/smgr.h"
#include "utils/elog.h"
#include "utils/rel.h"

#include "pg_tre/buffer.h"
#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"

void
pg_tre_page_init(Page page, Size page_size, PageTreKind kind)
{
    PageTreOpaque opq;

    PageInit(page, page_size, sizeof(PageTreOpaqueData));
    opq = PageTreGetOpaque(page);
    opq->page_kind      = (uint16) kind;
    opq->flags          = 0;
    /* New pages -- including pages rewritten by pg_tre_upgrade_index --
     * are always in the LATEST format. */
    opq->format_version = PG_TRE_FORMAT_VERSION_LATEST;
}

Buffer
pg_tre_extend(Relation index, PageTreKind kind)
{
    return pg_tre_extend_fork(index, MAIN_FORKNUM, kind);
}

Buffer
pg_tre_extend_fork(Relation index, ForkNumber forknum, PageTreKind kind)
{
    Buffer buf;
    Page   page;

    /*
     * For the main fork, first try to reuse a page previously freed to
     * the index FSM (a posting leaf reclaimed by VACUUM's deferred-recycle
     * pass).  GetFreeIndexPage returns InvalidBlockNumber when the FSM has
     * nothing on offer, in which case we physically extend the relation.
     *
     * The FSM is advisory: under concurrency it may hand the same block to
     * two extenders (e.g. an INSERT extending the pending list under the
     * meta-page lock racing VACUUM's merge under ShareUpdateExclusiveLock).
     * Following nbtree's _bt_allocbuf discipline, we therefore RE-VALIDATE
     * the candidate under its buffer lock: a genuinely free page is either
     * brand new (never written) or an initialized-but-empty page (the
     * blank state the recycle pass leaves behind -- pd_lower at the page
     * header end, no tuples).  If a peer already claimed and populated it,
     * we drop it and ask the FSM for the next candidate; if the FSM is
     * exhausted we fall through to a physical extension.  This closes the
     * double-hand-out window without relying on a relation-extension lock.
     */
    if (forknum == MAIN_FORKNUM)
    {
        BlockNumber reuse;

        while (BlockNumberIsValid((reuse = GetFreeIndexPage(index))))
        {
            CHECK_FOR_INTERRUPTS();

            buf = ReadBuffer(index, reuse);
            LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
            page = BufferGetPage(buf);

            /*
             * Reusable iff still unclaimed: a never-written page, or an
             * empty initialized page (pd_lower has not advanced past the
             * page header -- no content was written by a racing extender).
             */
            if (PageIsNew(page) ||
                ((PageHeader) page)->pd_lower <= SizeOfPageHeaderData)
            {
                pg_tre_page_init(page, BufferGetPageSize(buf), kind);
                MarkBufferDirty(buf);
                return buf;
            }

            /* Lost the race for this block; release and try the next. */
            UnlockReleaseBuffer(buf);
        }
    }

    buf = ExtendBufferedRel(BMR_REL(index), forknum, NULL,
                            EB_LOCK_FIRST);

    page = BufferGetPage(buf);
    pg_tre_page_init(page, BufferGetPageSize(buf), kind);

    MarkBufferDirty(buf);
    return buf;
}

Buffer
pg_tre_read(Relation index, BlockNumber blkno, PageTreKind expected_kind,
            int lock_mode)
{
    Buffer  buf = ReadBuffer(index, blkno);
    Page    page;

    LockBuffer(buf, lock_mode);
    page = BufferGetPage(buf);

    if (!PageIsNew(page))
    {
        PageTreOpaque opq = PageTreGetOpaque(page);

        /*
         * Accept any per-page format_version in the supported range.
         * Future format-dependent decoders (see src/util/bloom.c, the
         * apply_tuple_bloom_filter dispatch) branch on this value.
         * pg_tre_upgrade_index() rewrites pages forward to LATEST.
         */
        if (opq->format_version < PG_TRE_FORMAT_VERSION_MIN ||
            opq->format_version > PG_TRE_FORMAT_VERSION_LATEST)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("pg_tre: index %u page %u has format version %u, "
                            "supported range is [%u, %u]",
                            RelationGetRelid(index), blkno,
                            opq->format_version,
                            (uint32) PG_TRE_FORMAT_VERSION_MIN,
                            (uint32) PG_TRE_FORMAT_VERSION_LATEST)));

        if (expected_kind != PG_TRE_PAGE_INVALID &&
            opq->page_kind != (uint16) expected_kind)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("pg_tre: page %u of index %u has kind %u, "
                            "expected %u",
                            blkno, RelationGetRelid(index),
                            opq->page_kind,
                            (uint16) expected_kind)));
    }

    return buf;
}
