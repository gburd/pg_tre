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
#include "storage/bufpage.h"
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
/* Returns the pending list's entry count after the append. */
extern uint64 pg_tre_pending_append_batch(Relation index,
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
 * of entries merged.  The caller holds the merge lock (below).
 */
extern uint64 pg_tre_pending_merge(Relation index);

/*
 * The merge lock serializes everything that rewrites the trees or the
 * run catalog -- a pending-list merge, Hanoi merge / collapse, free-log
 * drain -- as GIN's LockPage on its metapage does.  VACUUM holds it for
 * its whole ambulkdelete / amvacuumcleanup pass (VACUUMs of one table are
 * serialized anyway); an INSERT that pushed the list past
 * pending_list_limit only tries it, and leaves the merge to whoever
 * holds it.  It is a heavyweight page lock on the meta block, which no
 * reader or appender takes, so it never blocks scans or inserts.
 */
extern void pg_tre_merge_lock(Relation index);
extern bool pg_tre_merge_trylock(Relation index);
extern void pg_tre_merge_unlock(Relation index);

/*
 * INSERT side of pending_list_limit: if the list holds more than the
 * limit (pg_tre_get_pending_list_limit_kb; 0 = no limit) and no merge is
 * running, merge it now.
 */
extern void pg_tre_pending_maybe_merge(Relation index, uint64 n_entries);

/*
 * Phase B1.4: collapse all live runs (base + catalog) into a single
 * fresh base run, bounding run growth so a quiescent index converges
 * to the single-structure layout.  Driven from VACUUM.  Returns the
 * number of runs collapsed (0 when already single-run).
 */
extern uint32 pg_tre_collapse_runs(Relation index);

/*
 * Phase B1.4 Hanoi incremental merge: merge one over-capacity Hanoi
 * level at a time (level L holds <= 2^(L-1) runs) into a promoted
 * level-(L+1) run, bounding per-merge work to a single level rather
 * than folding the whole catalog at once.  Driven from VACUUM.
 * Returns the number of merge passes performed.
 */
extern uint32 pg_tre_hanoi_merge(Relation index);


#endif /* PG_TRE_PENDING_H */
