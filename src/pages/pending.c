/*
 * src/pages/pending.c - fast-update pending list.
 *
 * Phase 4: append-only page chain holding (trigram_hash, tid, position)
 * entries from recent inserts.  Merge sweep consumes the list and
 * updates posting trees.  Scans union the pending list into results
 * to preserve consistency pre-merge.
 *
 * ---- Phase 2 stub ----
 *
 * For Phase 2 (build-only), the pending list is not used.  The meta
 * page initializes head/tail to InvalidBlockNumber.  This file provides
 * stub functions with clear TODO markers for Phase 4.
 */

#include "postgres.h"

#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"

/*
 * Initialize an empty pending list (no-op for Phase 2, since
 * pg_tre_meta_init already sets head=tail=InvalidBlockNumber).
 */
void
pg_tre_pending_init_empty(Relation index)
{
    /* Nothing to do; meta page already has InvalidBlockNumber. */
}

/*
 * Append one entry to the pending list.  Phase 4 allocates a tail
 * page if needed, packs the entry, WAL-logs, and updates meta.
 */
void
pg_tre_pending_append(Relation index, uint64 trigram_hash, ItemPointer tid,
                      uint32 position)
{
    /*
     * Phase 4: extend pending list tail page, pack PgTrePendingEntry,
     * update meta.pending_tail and meta.pending_n_entries, WAL-log.
     */
    elog(ERROR, "pg_tre: pending list not yet implemented (Phase 4)");
}

/*
 * Iterate over pending-list entries, invoking callback for each.
 * Phase 4 implements sequential scan of the page chain from head to tail.
 */
void
pg_tre_pending_scan(Relation index,
                    void (*callback)(uint64 hash, ItemPointer tid,
                                     uint32 position, void *ctx),
                    void *ctx)
{
    /*
     * Phase 4: walk pages from meta.pending_head to meta.pending_tail,
     * unpacking PgTrePendingEntry[] and invoking callback.
     */
    elog(ERROR, "pg_tre: pending list scan not yet implemented (Phase 4)");
}

/*
 * Consume the pending list and merge it into posting trees.  Phase 4
 * implements sort-merge bulk update with WAL consistency.
 */
void
pg_tre_pending_merge(Relation index)
{
    /*
     * Phase 4: read pending list, sort by trigram_hash, update posting
     * trees in batch, WAL-log merge, reset meta.pending_head/tail.
     */
    elog(ERROR, "pg_tre: pending list merge not yet implemented (Phase 4)");
}
