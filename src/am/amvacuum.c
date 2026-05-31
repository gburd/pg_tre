/*
 * src/am/amvacuum.c - VACUUM hooks.
 *
 *   - ambulkdelete: walk every posting tree reachable from the upper
 *     tree and strip dead heap TIDs (issue C2).  For each stored TID we
 *     invoke the supplied IndexBulkDeleteCallback; TIDs reported dead
 *     are removed from the leaf sparsemap and their parallel payload
 *     entries are dropped, and the leaf is WAL-logged.  Real statistics
 *     are reported (tuples_removed / num_index_tuples / num_pages).
 *   - amvacuumcleanup: merge the pending list into posting trees and
 *     report index-size statistics.
 *
 * INLINE postings stored directly in upper-tree leaf entries are also
 * cleaned as of 1.5.4 (posting_leaf_inline_delete in posting.c rewrites
 * the leaf's inline region in place), so num_index_tuples is exact.
 *
 * Residual gap: emptied posting leaves are repacked but not physically
 * freed to the free-space map.  Safe reclaim would require an
 * nbtree-style page deletion / recycle protocol (half-dead marking,
 * right-link re-routing, XID-gated recycling) because VACUUM runs
 * concurrently with lock-coupling-free right-link scans; that is tracked
 * future work.  pages_deleted / pages_free therefore stay 0 (honest).
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "commands/vacuum.h"
#include "storage/bufmgr.h"
#include "utils/elog.h"
#include "utils/rel.h"

#include "pg_tre/amapi.h"
#include "pg_tre/pending.h"
#include "pg_tre/posting.h"

IndexBulkDeleteResult *
pg_tre_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                    IndexBulkDeleteCallback callback, void *callback_state)
{
    uint64      removed;
    uint64      remaining = 0;
    BlockNumber posting_pages = 0;

    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

    if (info->index == NULL)
        return stats;

    /*
     * C2: strip dead TIDs from every posting tree reachable via the
     * upper tree.  The callback decides which TIDs are dead; removed
     * leaves are repacked in place and WAL-logged.
     */
    removed = pg_tre_posting_bulk_delete(info->index, callback,
                                         callback_state,
                                         &remaining, &posting_pages);

    /*
     * Accumulate across the (possibly multiple) ambulkdelete calls VACUUM
     * makes for one index.  tuples_removed is additive; num_index_tuples
     * tracks the most recent surviving-count observation.  Each call
     * re-walks the whole index (out-of-line posting trees AND inline
     * postings), so the latest `remaining` is the authoritative exact
     * live-tuple count -- num_index_tuples is no longer an estimate.
     */
    stats->tuples_removed += (double) removed;
    stats->num_index_tuples = (double) remaining;
    stats->estimated_count = false;

    /* Total physical pages in the index relation's main fork. */
    stats->num_pages = RelationGetNumberOfBlocks(info->index);

    return stats;
}

IndexBulkDeleteResult *
pg_tre_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    uint64 merged;

    /* Nothing to scan (e.g. cleanup-only with an all-visible heap). */
    if (info->index == NULL)
        return stats;

    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

    /* Drain the pending list into the posting trees. */
    merged = pg_tre_pending_merge(info->index);
    (void) merged;    /* reporting via ereport would spam VACUUM output */

    /*
     * Report index-size statistics.  We always know the physical page
     * count; report it so the planner / autovacuum logging see a real
     * value instead of zero.  pg_tre does not currently maintain a
     * free-space map or mark pages deleted, so pages_deleted / pages_free
     * stay zero (honestly reflecting that no pages are reclaimed without
     * a REINDEX).
     */
    stats->num_pages = RelationGetNumberOfBlocks(info->index);

    /*
     * If ambulkdelete already ran in this VACUUM it left num_index_tuples
     * populated; preserve it.  When cleanup runs standalone (no prior
     * bulkdelete, e.g. a cleanup-only VACUUM), leave the estimate flag set
     * so the planner treats the (possibly stale) tuple count as inexact.
     */
    if (stats->num_index_tuples == 0 && stats->tuples_removed == 0)
        stats->estimated_count = true;

    return stats;
}
