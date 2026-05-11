/*
 * include/pg_tre/pending.h - fast-update pending list API.
 *
 * The pending list is a forward-linked chain of pages storing
 * (trigram_hash, tid, position) triples from recent inserts.  Writers
 * append at the tail; the list is drained on VACUUM or by an explicit
 * pg_tre_flush() call, at which point entries are merged into the
 * per-trigram posting trees.
 *
 * Readers (amgetbitmap) union the pending list into their results so
 * that a newly-inserted TID is visible even before the next merge.
 */

#ifndef PG_TRE_PENDING_H
#define PG_TRE_PENDING_H

#include "postgres.h"

#include "storage/itemptr.h"
#include "utils/rel.h"

/*
 * Append one (trigram_hash, tid, position) entry to the pending list.
 * Allocates a new tail page when the current tail is full; WAL-logs
 * the insert and the metapage update.  Caller must have an exclusive
 * lock on its own row (we are called from aminsert).
 */
extern void pg_tre_pending_append(Relation index,
                                  uint64 trigram_hash,
                                  ItemPointer tid,
                                  uint32 position);

/*
 * Batch variant: append N entries in one go.  Reduces WAL overhead
 * and buffer-lock churn during a multi-trigram insert.  `n` must be
 * <= PG_TRE_PENDING_BATCH_MAX (defined below).
 */
#define PG_TRE_PENDING_BATCH_MAX 256
extern void pg_tre_pending_append_batch(Relation index,
                                        const uint64 *hashes,
                                        const ItemPointerData *tids,
                                        const uint32 *positions,
                                        int n);

/*
 * Iterate all pending-list entries.  `callback` receives each triple
 * in insertion order.  Holds a share lock on one page at a time.
 */
typedef void (*PgTrePendingCallback)(uint64 trigram_hash,
                                     ItemPointer tid,
                                     uint32 position,
                                     void *ctx);

extern void pg_tre_pending_scan(Relation index,
                                PgTrePendingCallback callback,
                                void *ctx);

/*
 * Merge the entire pending list into posting trees, then truncate
 * the list.  Called by VACUUM (amvacuumcleanup).  Returns the number
 * of entries merged.
 */
extern uint64 pg_tre_pending_merge(Relation index);

#endif /* PG_TRE_PENDING_H */
