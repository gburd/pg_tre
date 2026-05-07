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

#define XLOG_PTRE_OPMASK             0xF0

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

/* Rmgr hooks — all in src/wal/xlog.c */
extern void pg_tre_redo(XLogReaderState *record);
extern void pg_tre_desc(StringInfo buf, XLogReaderState *record);
extern const char *pg_tre_identify(uint8 info);
extern void pg_tre_startup(void);
extern void pg_tre_cleanup(void);
extern void pg_tre_mask(char *pagedata, BlockNumber blkno);

#endif /* PG_TRE_XLOG_H */
