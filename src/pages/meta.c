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
    meta->format_version     = PG_TRE_FORMAT_VERSION_LATEST;
    meta->min_page_format_version = PG_TRE_FORMAT_VERSION_LATEST;
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
     * Phase B1 (v7) run catalog: a fresh index has one implicit run
     * rooted at root_upper/root_range, so there is no catalog page.
     * Stamp the fields explicitly (rather than rely on read-time
     * normalization) so the on-disk page is self-describing.
     */
    meta->next_run_id        = 1;
    meta->run_catalog_head   = InvalidBlockNumber;
    meta->n_runs             = 0;
    meta->max_levels         = 7;

    /*
     * Blocker 2 deferred page-free log: a fresh index has no logged
     * pages.  Stamp explicitly so the on-disk page is self-describing.
     */
    meta->free_log_head      = InvalidBlockNumber;
    meta->_pad_free_log      = 0;

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

    /*
     * Indexes built on 1.3.x and earlier left this slot zero in the
     * reserved[] tail.  Treat zero as "unknown, assume index-level
     * format_version".  pg_tre_upgrade_index() will set it explicitly
     * after a sweep.
     */
    if (out->min_page_format_version == 0)
        out->min_page_format_version = out->format_version;

    /*
     * Phase B1 (v7) run catalog.  A pre-v7 (v6) meta page left these
     * fields zero in the former reserved[] tail.  Normalize a v6 (or
     * un-initialized v7) page to the single-implicit-run state:
     * run_catalog_head = InvalidBlockNumber (NOT 0, which is a valid
     * block), n_runs = 0 (meaning "one implicit run rooted at
     * root_upper/root_range"), and a default Hanoi cap.  next_run_id
     * starts at 1 so the first real flushed run gets a nonzero id.
     */
    if (out->n_runs == 0 && out->run_catalog_head == 0)
        out->run_catalog_head = InvalidBlockNumber;
    if (out->max_levels == 0)
        out->max_levels = 7;
    if (out->next_run_id == 0)
        out->next_run_id = 1;

    /*
     * Blocker 2 free log.  An index built before this feature left this
     * slot zero in the former reserved[] tail; 0 is a valid block
     * number, so we cannot distinguish "head is block 0" from "unset".
     * Block 0 is always the meta page and can never be a free-log page,
     * so a zero here unambiguously means "no free log" ->
     * InvalidBlockNumber.
     */
    if (out->free_log_head == 0)
        out->free_log_head = InvalidBlockNumber;

    UnlockReleaseBuffer(buf);
}

void
pg_tre_build_empty(Relation index)
{
    pg_tre_build_empty_fork(index, MAIN_FORKNUM);
}

void
pg_tre_build_empty_fork(Relation index, ForkNumber forknum)
{
    Buffer metabuf;
    Page   metapage;
    bool   wal_log;

    /* Allocate block 0 as the meta page in the requested fork. */
    metabuf = pg_tre_extend_fork(index, forknum, PG_TRE_PAGE_META);
    Assert(BufferGetBlockNumber(metabuf) == PG_TRE_META_BLKNO);

    metapage = BufferGetPage(metabuf);
    pg_tre_meta_init(metapage);

    /*
     * WAL-log the meta-page init.  Two paths:
     *
     *   - For the main fork: log only if the relation needs WAL.
     *     UNLOGGED relations don't WAL-log their main fork.
     *
     *   - For the init fork: log unconditionally.  The init fork is
     *     the WAL-logged template that gets copied to the main fork
     *     during crash recovery, so it MUST be replayable even on
     *     UNLOGGED indexes (where RelationNeedsWAL returns false).
     */
    wal_log = (forknum == INIT_FORKNUM) || RelationNeedsWAL(index);

    MarkBufferDirty(metabuf);

    /* MarkBufferDirty must precede XLogRegisterBuffer (PG18 asserts
     * buffer is dirty + exclusively locked). */
    if (wal_log)
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, metabuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_META_UPDATE);
        PageSetLSN(metapage, recptr);
    }

    UnlockReleaseBuffer(metabuf);

    /*
     * For the init fork, smgr fsync immediately so the template is
     * durable before the surrounding CREATE INDEX commits.  This
     * matches what btbuildempty does for the same reason.
     */
    if (forknum == INIT_FORKNUM)
    {
        smgrimmedsync(RelationGetSmgr(index), forknum);
    }
}

void
pg_tre_meta_set_roots(Relation index, BlockNumber root_upper,
                      BlockNumber root_range, uint64 n_trigrams,
                      uint64 n_tuples_indexed)
{
    Buffer metabuf;
    Page   metapage;
    PgTreMetaPage meta;

    metabuf = pg_tre_read(index, PG_TRE_META_BLKNO, PG_TRE_PAGE_META,
                          BUFFER_LOCK_EXCLUSIVE);
    metapage = BufferGetPage(metabuf);
    meta = PgTreMetaPageGet(metapage);

    /* Update tree roots and stats. */
    meta->root_upper = root_upper;
    meta->root_range = root_range;
    meta->n_trigrams = n_trigrams;
    meta->n_tuples_indexed = n_tuples_indexed;

    MarkBufferDirty(metabuf);

    /* WAL-log the update.  MarkBufferDirty must precede
     * XLogRegisterBuffer (PG18 asserts buffer is dirty + exclusively
     * locked). */
    if (RelationNeedsWAL(index))
    {
        XLogRecPtr recptr;

        XLogBeginInsert();
        XLogRegisterBuffer(0, metabuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);

        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_META_UPDATE);
        PageSetLSN(metapage, recptr);
    }

    UnlockReleaseBuffer(metabuf);
}
