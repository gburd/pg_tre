/*
 * include/pg_tre/meta.h - meta page access.
 */

#ifndef PG_TRE_META_H
#define PG_TRE_META_H

#include "postgres.h"
#include "storage/block.h"
#include "common/relpath.h"
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

/*
 * Same as pg_tre_build_empty but populates the named fork.  Used by
 * pg_tre_ambuildempty to set up the init fork for UNLOGGED indexes:
 * the init fork is the WAL-logged template that gets copied to the
 * main fork during crash recovery.  WAL logging fires unconditionally
 * for INIT_FORKNUM regardless of RelationNeedsWAL, since the init
 * fork must be replayable even on UNLOGGED indexes.
 */
extern void pg_tre_build_empty_fork(Relation index, ForkNumber forknum);

/*
 * Update the meta page's root pointers and stats.  Used by ambuild
 * after bulk-loading the upper and range trees.
 */
extern void pg_tre_meta_set_roots(Relation index, BlockNumber root_upper,
                                  BlockNumber root_range, uint64 n_trigrams,
                                  uint64 n_tuples_indexed);

#endif /* PG_TRE_META_H */
