/*
 * include/pg_tre/meta.h - meta page access.
 */

#ifndef PG_TRE_META_H
#define PG_TRE_META_H

#include "postgres.h"
#include "storage/block.h"
#include "utils/rel.h"

#include "pg_tre/page.h"

/*
 * Initialize a freshly-extended meta page with default field values.
 * Caller holds the exclusive buffer lock on the meta page.
 */
extern void pg_tre_meta_init(Page page);

/*
 * Read the meta page with shared lock, copy its contents into *out,
 * release the lock and return.  Short-lived -- callers that need to
 * re-read must call again rather than caching the result across
 * buffer-lock release.
 */
extern void pg_tre_meta_read(Relation index, PgTreMetaPageData *out);

/*
 * Build an empty index: meta page (block 0) only.  Invoked by
 * ambuildempty.  Emits WAL record XLOG_PTRE_META_UPDATE if the index
 * is WAL-logged (standard nbtree pattern).
 */
extern void pg_tre_build_empty(Relation index);

#endif /* PG_TRE_META_H */
