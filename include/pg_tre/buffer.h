/*
 * include/pg_tre/buffer.h - buffer-access helpers for the pg_tre AM.
 *
 * Wraps Postgres buffer APIs with pg_tre-specific defaults: pages are
 * always initialized with the right opaque trailer, locking follows a
 * consistent meta -> upper -> posting -> range -> pending order, and
 * page-kind assertions fire in assert-enabled builds.
 */

#ifndef PG_TRE_BUFFER_H
#define PG_TRE_BUFFER_H

#include "postgres.h"
#include "access/genam.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"

#include "pg_tre/page.h"

/*
 * Initialize a newly-allocated page as a given page kind.  The caller
 * still holds the exclusive buffer lock; no WAL is emitted here.
 */
extern void pg_tre_page_init(Page page, Size page_size, PageTreKind kind);

/*
 * Allocate a new buffer in the index relation, initialize it as the
 * given kind, mark it dirty and return it with exclusive lock held.
 * Phase 1 uses this during ambuildempty; Phase 2+ uses it throughout
 * build and incremental allocation.
 */
extern Buffer pg_tre_extend(Relation index, PageTreKind kind);

/*
 * Read an existing page with lock of the requested mode (BUFFER_LOCK_SHARE
 * or BUFFER_LOCK_EXCLUSIVE).  Asserts that the page kind matches the
 * expected kind.
 */
extern Buffer pg_tre_read(Relation index, BlockNumber blkno,
                          PageTreKind expected_kind, int lock_mode);

#endif /* PG_TRE_BUFFER_H */
