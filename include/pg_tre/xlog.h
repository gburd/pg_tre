/*
 * include/pg_tre/xlog.h - WAL record declarations for the pg_tre rmgr.
 *
 * pg_tre registers a custom resource manager via RegisterCustomRmgr
 * (PG15+). On PG18, the RmgrId range 128..255 is reserved for extensions;
 * we use RM_PG_TRE_ID = 140 by default, override at build time if it
 * collides with another extension in your cluster.
 */

#ifndef PG_TRE_XLOG_H
#define PG_TRE_XLOG_H

#include "postgres.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"

#ifndef RM_PG_TRE_ID
#define RM_PG_TRE_ID 140
#endif

#define RM_PG_TRE_NAME "pg_tre"

/* Record types — stored in xl_info upper 4 bits of xl_rmid_info. */
#define XLOG_PTRE_META_UPDATE        0x10
#define XLOG_PTRE_UPPER_INSERT       0x20
#define XLOG_PTRE_UPPER_SPLIT        0x30
#define XLOG_PTRE_POSTING_INSERT     0x40
#define XLOG_PTRE_POSTING_DELETE     0x50
#define XLOG_PTRE_POSTING_SPLIT      0x60
#define XLOG_PTRE_RANGE_UPDATE       0x70
#define XLOG_PTRE_PENDING_INSERT     0x80
#define XLOG_PTRE_PENDING_MERGE_B    0x90
#define XLOG_PTRE_PENDING_MERGE_C    0xA0
#define XLOG_PTRE_VACUUM             0xB0
#define XLOG_PTRE_PAGE_FORMAT_UPGRADE 0xC0
#define XLOG_PTRE_POSTING_UNLINK     0xD0
#define XLOG_PTRE_POSTING_RECYCLE    0xE0

#define XLOG_PTRE_OPMASK             0xF0

/*
 * Low 4 bits of xl_info are reserved for variant flags.
 * XLOG_PTRE_DELTA_FLAG distinguishes delta-encoded records from
 * the corresponding FPI-only variant.  Delta records register
 * per-block payload via XLogRegisterBufData; redo decodes the
 * payload and applies it to the buffer obtained from
 * XLogReadBufferForRedo.  When the flag is absent, the record is
 * a full-page image and pg_tre_redo_fpi handles replay.
 */
#define XLOG_PTRE_FLAGMASK           0x0F
#define XLOG_PTRE_DELTA_FLAG         0x01

/* Record-body structs -- all are followed by variable-length payload. */

typedef struct xl_pg_tre_meta_update
{
    uint32      format_version;
    /* followed by full meta image */
} xl_pg_tre_meta_update;

typedef struct xl_pg_tre_posting_insert
{
    uint64      trigram_hash;
    uint32      n_entries;
    /* followed by sparsemap delta blob + optional payload records */
} xl_pg_tre_posting_insert;

typedef struct xl_pg_tre_pending_insert
{
    uint32      n_entries;
    /* followed by PgTrePendingEntry[n_entries] */
} xl_pg_tre_pending_insert;

/*
 * Per-block payload registered via XLogRegisterBufData for the
 * delta variant of XLOG_PTRE_PENDING_INSERT.
 *
 * Block 0 (metabuf): xl_pg_tre_pending_insert_meta_delta
 *   - on redo, add `take` to PgTreMetaPageData->pending_n_entries.
 *
 * Block 1 (tailbuf): xl_pg_tre_pending_insert_tail_delta followed by
 *   `take * sizeof(PgTrePendingEntry)` bytes of entry data.
 *   - on redo, append the entries at slot `prev_n_entries`.
 *
 * The delta variant is only emitted when the tail page already
 * exists on disk (tail_is_new == false).  When the primary just
 * extended a fresh tail page, the standby cannot reconstruct the
 * page-init layout from a delta, so the FPI variant is emitted
 * instead.
 */
typedef struct xl_pg_tre_pending_insert_meta_delta
{
    uint32      n_entries_added;
} xl_pg_tre_pending_insert_meta_delta;

typedef struct xl_pg_tre_pending_insert_tail_delta
{
    uint16      prev_n_entries;
    uint16      take;
    /* followed by PgTrePendingEntry[take] */
} xl_pg_tre_pending_insert_tail_delta;

/* Rmgr hooks — all in src/wal/xlog.c */
extern void pg_tre_redo(XLogReaderState *record);
extern void pg_tre_desc(StringInfo buf, XLogReaderState *record);
extern const char *pg_tre_identify(uint8 info);
extern void pg_tre_startup(void);
extern void pg_tre_cleanup(void);
extern void pg_tre_mask(char *pagedata, BlockNumber blkno);

#endif /* PG_TRE_XLOG_H */
