/*
 * src/pages/meta.c - meta page reader/writer.
 *
 * Meta page is always block 0 of the index.  It carries format
 * version, tree roots, pending-list pointers, and index-wide stats.
 * The layout is declared in include/pg_tre/page.h and serialized
 * byte-exactly; no host-endian conversion is performed.
 */

#include "postgres.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lockdefs.h"
#include "storage/smgr.h"
#include "utils/elog.h"
#include "utils/rel.h"

#include "pg_tre/buffer.h"
#include "pg_tre/meta.h"
#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/xlog.h"

void
pg_tre_meta_init(Page page)
{
    PgTreMetaPage meta;

    pg_tre_page_init(page, BLCKSZ, PG_TRE_PAGE_META);

    meta = PgTreMetaPageGet(page);
    memset(meta, 0, sizeof(*meta));

    meta->magic              = PG_TRE_META_MAGIC;
    meta->format_version     = PG_TRE_FORMAT_VERSION;
    meta->q                  = 3;
    meta->tri_encoding       = 0;                /* byte trigrams for now */
    meta->bloom_range_m_bits = (uint32) pg_tre_bloom_tuple_bits * 16;
    meta->bloom_range_k      = 7;
    meta->bloom_tuple_m_bits = (uint16) pg_tre_bloom_tuple_bits;
    meta->bloom_tuple_k      = 5;

    meta->root_upper         = InvalidBlockNumber;
    meta->root_range         = InvalidBlockNumber;
    meta->pending_head       = InvalidBlockNumber;
    meta->pending_tail       = InvalidBlockNumber;
    meta->pending_n_entries  = 0;
    meta->stats_blk          = InvalidBlockNumber;
    meta->n_trigrams         = 0;
    meta->n_tuples_indexed   = 0;
    meta->created_xid        = GetCurrentTransactionIdIfAny();

    /*
     * Place the lower/upper pointers past the meta struct so PageAddItem
     * (which we don't use on this page, but which PageInit expects to be
     * valid) remains self-consistent.
     */
    ((PageHeader) page)->pd_lower =
        ((char *) meta - (char *) page) + sizeof(*meta);
}

void
pg_tre_meta_read(Relation index, PgTreMetaPageData *out)
{
    Buffer buf = pg_tre_read(index, PG_TRE_META_BLKNO, PG_TRE_PAGE_META,
                             BUFFER_LOCK_SHARE);
    Page   page = BufferGetPage(buf);
    PgTreMetaPage meta = PgTreMetaPageGet(page);

    memcpy(out, meta, sizeof(*meta));

    if (out->magic != PG_TRE_META_MAGIC)
    {
        UnlockReleaseBuffer(buf);
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_tre: meta page magic mismatch (got 0x%08X)",
                        out->magic)));
    }

    UnlockReleaseBuffer(buf);
}

void
pg_tre_build_empty(Relation index)
{
    Buffer metabuf;
    Page   metapage;

    /* Allocate block 0 as the meta page. */
    metabuf = pg_tre_extend(index, PG_TRE_PAGE_META);
    Assert(BufferGetBlockNumber(metabuf) == PG_TRE_META_BLKNO);

    metapage = BufferGetPage(metabuf);
    pg_tre_meta_init(metapage);

    /* WAL-log the meta-page init as a full-page image. */
    if (RelationNeedsWAL(index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_META_UPDATE);
        PageSetLSN(metapage, recptr);
    }

    MarkBufferDirty(metabuf);
    UnlockReleaseBuffer(metabuf);
}
