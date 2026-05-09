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

#include "access/xlog.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "access/xlogutils.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "utils/elog.h"

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
 * (REGBUF_WILL_INIT + REGBUF_STANDARD).  XLogReadBufferForRedo will
 * reconstitute the page from the backup block; we only need to mark
 * it dirty and release.
 */
static void
pg_tre_redo_fpi(XLogReaderState *record)
{
    int     blkno;
    Buffer  buf;

    for (blkno = 0; blkno < XLogRecMaxBlockId(record); blkno++)
    {
        XLogRedoAction action = XLogReadBufferForRedo(record, blkno, &buf);

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

void
pg_tre_redo(XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & XLOG_PTRE_OPMASK;

    switch (info)
    {
        case XLOG_PTRE_META_UPDATE:
            pg_tre_redo_fpi(record);
            break;

        case XLOG_PTRE_UPPER_INSERT:
        case XLOG_PTRE_UPPER_SPLIT:
        case XLOG_PTRE_POSTING_INSERT:
        case XLOG_PTRE_POSTING_DELETE:
        case XLOG_PTRE_POSTING_SPLIT:
        case XLOG_PTRE_RANGE_UPDATE:
        case XLOG_PTRE_PENDING_INSERT:
        case XLOG_PTRE_PENDING_MERGE_B:
        case XLOG_PTRE_PENDING_MERGE_C:
        case XLOG_PTRE_VACUUM:
            elog(PANIC, "pg_tre: redo for info 0x%02X not yet implemented",
                 info);
            break;

        default:
            elog(PANIC, "pg_tre_redo: unknown xl_info 0x%02X", info);
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

void
pg_tre_mask(char *pagedata, BlockNumber blkno)
{
    /*
     * Phase 7 implements buffer masking for wal_consistency_checking.
     * The mask zeroes out fields that legitimately differ between
     * primary and replica (hint bits, etc.).
     */
}
