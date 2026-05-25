/*
 * src/wal/xlog.c - custom resource manager for pg_tre.
 *
 * Phase 1 wires up the rmgr registration and all hook signatures; the
 * redo and desc bodies are real when a record type is used by its
 * corresponding page-layer code.  Until a record type is emitted, its
 * redo branch remains unreachable and triggers an elog(PANIC) as a
 * correctness guard.
 */

#include "postgres.h"

#include "access/bufmask.h"
#include "access/xlog.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "access/xlogutils.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/elog.h"

#include "pg_tre/page.h"
#include "pg_tre/pending.h"
#include "pg_tre/xlog.h"

void
pg_tre_startup(void)
{
    /* Called at the start of WAL redo.  No per-rmgr state yet. */
}

void
pg_tre_cleanup(void)
{
    /* Called at the end of WAL redo. */
}

/*
 * Generic redo for records that were logged as full-page images
 * (REGBUF_STANDARD).  XLogReadBufferForRedo applies the FPI; we
 * just need to mark it dirty and release.
 *
 * Records that ship deltas are dispatched separately (e.g.
 * pg_tre_redo_pending_insert_delta).  Records still emitted as
 * FPI-only land here.
 */
static void
pg_tre_redo_fpi(XLogReaderState *record)
{
    int     blkno;
    Buffer  buf;

    /*
     * XLogRecMaxBlockId returns the maximum block_id used in the
     * record, NOT the count.  For a record that registered buffers
     * with IDs 0 and 1 (typical: meta page + page-being-modified),
     * the max is 1 and we need to iterate blkno = 0..1 inclusive.
     * The previous `blkno < max` form silently dropped the highest-
     * numbered block on every redo — caught by replication.sh on a
     * primary/standby pair where the standby rejected the next
     * write to block 52 because its in-memory state for block 52
     * hadn't been replayed.
     */
    for (blkno = 0; blkno <= XLogRecMaxBlockId(record); blkno++)
    {
        XLogRedoAction action;

        if (!XLogRecHasBlockRef(record, blkno))
            continue;

        action = XLogReadBufferForRedo(record, blkno, &buf);
        if (action == BLK_NEEDS_REDO)
        {
            /*
             * The record carries no delta -- a full-page image is
             * enough to restore.  XLogReadBufferForRedo already
             * applied it; just release.
             */
            PageSetLSN(BufferGetPage(buf), record->EndRecPtr);
            MarkBufferDirty(buf);
        }
        if (BufferIsValid(buf))
            UnlockReleaseBuffer(buf);
    }
}

/*
 * Redo for the delta variant of XLOG_PTRE_PENDING_INSERT.
 *
 * The record carries:
 *   block 0 (meta): xl_pg_tre_pending_insert_meta_delta
 *   block 1 (tail): xl_pg_tre_pending_insert_tail_delta + entries
 *
 * For each block, XLogReadBufferForRedo returns one of:
 *   BLK_NEEDS_REDO   - apply the delta
 *   BLK_RESTORED     - FPI was applied (won't happen here, no FPI in
 *                      this record) but harmless.
 *   BLK_DONE         - already replayed, skip
 *   BLK_NOTFOUND     - block was truncated since, skip
 */
static void
pg_tre_redo_pending_insert_delta(XLogReaderState *record)
{
    Buffer  metabuf;
    Buffer  tailbuf;
    Page    page;
    Size    bufdata_sz;
    char   *bufdata;
    XLogRedoAction action;

    /* Block 0: meta */
    action = XLogReadBufferForRedo(record, 0, &metabuf);
    if (action == BLK_NEEDS_REDO)
    {
        const xl_pg_tre_pending_insert_meta_delta *md;
        PgTreMetaPage m;

        bufdata = XLogRecGetBlockData(record, 0, &bufdata_sz);
        if (bufdata_sz != sizeof(*md))
            elog(PANIC,
                 "pg_tre redo PENDING_INSERT_DELTA: meta payload %zu != %zu",
                 bufdata_sz, sizeof(*md));
        md = (const xl_pg_tre_pending_insert_meta_delta *) bufdata;

        page = BufferGetPage(metabuf);
        m = PgTreMetaPageGet(page);
        m->pending_n_entries += md->n_entries_added;

        PageSetLSN(page, record->EndRecPtr);
        MarkBufferDirty(metabuf);
    }
    if (BufferIsValid(metabuf))
        UnlockReleaseBuffer(metabuf);

    /* Block 1: tail */
    action = XLogReadBufferForRedo(record, 1, &tailbuf);
    if (action == BLK_NEEDS_REDO)
    {
        const xl_pg_tre_pending_insert_tail_delta *td;
        const PgTrePendingEntry *src;
        Size    entries_sz;

        bufdata = XLogRecGetBlockData(record, 1, &bufdata_sz);
        if (bufdata_sz < sizeof(*td))
            elog(PANIC,
                 "pg_tre redo PENDING_INSERT_DELTA: tail payload %zu < %zu",
                 bufdata_sz, sizeof(*td));
        td = (const xl_pg_tre_pending_insert_tail_delta *) bufdata;
        entries_sz = (Size) td->take * sizeof(PgTrePendingEntry);
        if (bufdata_sz != sizeof(*td) + entries_sz)
            elog(PANIC,
                 "pg_tre redo PENDING_INSERT_DELTA: tail payload %zu != header+%zu",
                 bufdata_sz, entries_sz);
        src = (const PgTrePendingEntry *) (bufdata + sizeof(*td));

        page = BufferGetPage(tailbuf);
        if (!pg_tre_pending_redo_apply_delta(page, td->prev_n_entries,
                                             td->take, src))
            elog(PANIC,
                 "pg_tre redo PENDING_INSERT_DELTA: standby tail state "
                 "differs from primary at delta apply (expected prev=%u)",
                 td->prev_n_entries);

        PageSetLSN(page, record->EndRecPtr);
        MarkBufferDirty(tailbuf);
    }
    if (BufferIsValid(tailbuf))
        UnlockReleaseBuffer(tailbuf);
}

void
pg_tre_redo(XLogReaderState *record)
{
    uint8 info_full = XLogRecGetInfo(record);
    uint8 op        = info_full & XLOG_PTRE_OPMASK;
    uint8 flags     = info_full & XLOG_PTRE_FLAGMASK;

    switch (op)
    {
        case XLOG_PTRE_META_UPDATE:
            pg_tre_redo_fpi(record);
            break;

        case XLOG_PTRE_PENDING_INSERT:
            if (flags & XLOG_PTRE_DELTA_FLAG)
                pg_tre_redo_pending_insert_delta(record);
            else
                pg_tre_redo_fpi(record);
            break;

        case XLOG_PTRE_UPPER_INSERT:
        case XLOG_PTRE_UPPER_SPLIT:
        case XLOG_PTRE_POSTING_INSERT:
        case XLOG_PTRE_PENDING_MERGE_B:
        case XLOG_PTRE_PENDING_MERGE_C:
        case XLOG_PTRE_RANGE_UPDATE:      /* Phase 5: range tier FPI */
            /*
             * Phase 2/4/5 emit these as full-page images.  The generic
             * FPI replay is correct for all of them today; future
             * work can introduce delta records following the
             * pending_insert delta pattern above.
             */
            pg_tre_redo_fpi(record);
            break;

        case XLOG_PTRE_POSTING_DELETE:
        case XLOG_PTRE_POSTING_SPLIT:
        case XLOG_PTRE_VACUUM:
            elog(PANIC, "pg_tre: redo for op 0x%02X not yet implemented",
                 op);
            break;

        default:
            elog(PANIC, "pg_tre_redo: unknown op 0x%02X (info 0x%02X)",
                 op, info_full);
    }
}

void
pg_tre_desc(StringInfo buf, XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & XLOG_PTRE_OPMASK;
    appendStringInfo(buf, "op=0x%02X", info);
}

const char *
pg_tre_identify(uint8 info)
{
    switch (info & XLOG_PTRE_OPMASK)
    {
        case XLOG_PTRE_META_UPDATE:     return "META_UPDATE";
        case XLOG_PTRE_UPPER_INSERT:    return "UPPER_INSERT";
        case XLOG_PTRE_UPPER_SPLIT:     return "UPPER_SPLIT";
        case XLOG_PTRE_POSTING_INSERT:  return "POSTING_INSERT";
        case XLOG_PTRE_POSTING_DELETE:  return "POSTING_DELETE";
        case XLOG_PTRE_POSTING_SPLIT:   return "POSTING_SPLIT";
        case XLOG_PTRE_RANGE_UPDATE:    return "RANGE_UPDATE";
        case XLOG_PTRE_PENDING_INSERT:  return "PENDING_INSERT";
        case XLOG_PTRE_PENDING_MERGE_B: return "PENDING_MERGE_BEGIN";
        case XLOG_PTRE_PENDING_MERGE_C: return "PENDING_MERGE_COMMIT";
        case XLOG_PTRE_VACUUM:          return "VACUUM";
        default:                        return NULL;
    }
}

/*
 * Mask a pg_tre page before WAL consistency checks.
 *
 * Called on both the primary's full-page image (as captured at
 * XLogRegisterBuffer time, before XLogInsert returned the record's LSN)
 * and the standby's just-replayed page (whose LSN was advanced to
 * record->EndRecPtr by redo).  The mask must zero out fields that
 * legitimately differ so the bytewise comparison succeeds; it must NOT
 * mask user data or structural fields that real divergence would touch.
 *
 * Fields we mask, with rationale:
 *
 *   - pd_lsn / pd_checksum: The primary's FPI captures the page state
 *     BEFORE XLogInsert assigns this record's LSN.  After redo the
 *     standby sets pd_lsn = record->EndRecPtr.  pd_checksum is computed
 *     after every other byte is settled, so it follows pd_lsn.  Standard
 *     across all rmgrs.
 *
 *   - PageHeader hint flags + pd_prune_xid: PD_ALL_VISIBLE,
 *     PD_PAGE_FULL, PD_HAS_FREE_LINES are hint bits that can be set
 *     without WAL.  pg_tre does not use them today, but masking them
 *     is the standard defensive posture (heap, btree, gin all do it).
 *
 *   - Hole between pd_lower and pd_upper: REGBUF_STANDARD strips this
 *     region from the FPI on the primary and redo zeros it on the
 *     standby, so it should already match, but masking is cheap
 *     insurance against future code paths that touch the hole between
 *     XLogRegisterBuffer and the page being read for comparison.
 *
 * Things we deliberately do NOT mask:
 *
 *   - PageTreOpaqueData (page_kind, flags, format_version): all three
 *     are set at page-init time and only mutated by WAL-logged ops,
 *     so primary and standby must agree byte-for-byte.
 *
 *   - Per-page-type headers (PgTreMetaPageData, PgTrePostingLeafHeader,
 *     PgTrePendingHeader, PgTreUpperLeafEntry, PgTreRangeLeafEntry):
 *     these are the user-visible structural state of the index.  Any
 *     divergence here is a real redo bug, not a hint-bit quirk.
 *
 *   - Line-pointer flags: pg_tre stores its data in raw struct layouts
 *     in PageGetContents(), not via line pointers, so there are no
 *     LP_* flags to drift.
 */
void
pg_tre_mask(char *pagedata, BlockNumber blkno)
{
    Page page = (Page) pagedata;

    (void) blkno;

    mask_page_lsn_and_checksum(page);
    mask_page_hint_bits(page);

    /*
     * mask_unused_space asserts pd_lower / pd_upper are well-formed.
     * Skip the call on a freshly-extended (all-zero) page that has
     * never been initialized; such a page can transiently appear in
     * the buffer cache after smgr extension and before page init,
     * and PageIsNew is the same predicate the rest of the code uses
     * to detect it.
     */
    if (!PageIsNew(page))
        mask_unused_space(page);
}
