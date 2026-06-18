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
 * As of 1.5.5, emptied posting leaves ARE physically reclaimed: a
 * non-head leaf that empties is unlinked from its right-link chain and
 * marked deleted by ambulkdelete (deferred-recycle, nbtree-style), and
 * amvacuumcleanup hands deleted pages whose deletion XID has aged past
 * the global visibility horizon to the index free-space map
 * (pg_tre_posting_recycle_deleted).  pages_deleted / pages_free are
 * reported accordingly, and pg_tre_extend reuses freed pages.
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "commands/vacuum.h"
#include "storage/bufmgr.h"
#include "utils/elog.h"
#include "utils/rel.h"

#include "pg_tre/amapi.h"
#include "pg_tre/meta.h"
#include "pg_tre/pending.h"
#include "pg_tre/posting.h"
#include "pg_tre/run_catalog.h"

IndexBulkDeleteResult *
pg_tre_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                    IndexBulkDeleteCallback callback, void *callback_state)
{
    uint64      removed;
    uint64      remaining = 0;
    BlockNumber posting_pages = 0;
    BlockNumber pages_deleted = 0;

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
                                         &remaining, &posting_pages,
                                         &pages_deleted);

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

    /* Leaves unlinked + marked deleted this pass (recycled into the FSM
     * by amvacuumcleanup once their deletion XID ages out). */
    stats->pages_deleted += pages_deleted;

    /* Total physical pages in the index relation's main fork. */
    stats->num_pages = RelationGetNumberOfBlocks(info->index);

    return stats;
}

IndexBulkDeleteResult *
pg_tre_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    uint64      merged;
    BlockNumber recycled;
    BlockNumber pending = 0;

    /* Nothing to scan (e.g. cleanup-only with an all-visible heap). */
    if (info->index == NULL)
        return stats;

    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

    /* Drain the pending list into the posting trees. */
    merged = pg_tre_pending_merge(info->index);
    (void) merged;    /* reporting via ereport would spam VACUUM output */

    /*
     * Phase B1.4 compaction: VACUUM is the natural compaction point
     * for an index AM (no background thread).  Runs accrue across
     * VACUUMs (the LSM benefit); we bound their growth incrementally
     * via Hanoi level-merging -- each VACUUM merges at most one
     * over-capacity level (level L holds <= 2^(L-1) runs) into a
     * promoted run, so per-VACUUM work is bounded by a single level
     * rather than the whole catalog (which an all-at-once collapse of
     * hundreds of runs would make a multi-minute stall).
     *
     * As a backstop, if the catalog has grown well past what Hanoi
     * leveling should allow (e.g. a pathological burst that outran the
     * per-VACUUM merge budget, or max_levels exceeded), fall back to a
     * full collapse to single-run so scan cost can't degrade without
     * bound.  Both are no-ops when there is only the implicit run, so
     * the default (flush_to_run off) single-run case is unaffected.
     */
    {
        PgTreMetaPageData vmeta;
        uint32            cap, backstop;

        (void) pg_tre_hanoi_merge(info->index);

        pg_tre_meta_read(info->index, &vmeta);
        cap = (vmeta.max_levels > 0) ? vmeta.max_levels : 7;
        /* Backstop: total runs far above the Hanoi total capacity
         * (~2^max_levels) means leveling fell behind -- collapse. */
        backstop = (cap < 20) ? (1u << cap) : (1u << 20);
        if (pg_tre_run_count(info->index) > backstop)
            (void) pg_tre_collapse_runs(info->index);
    }

    /*
     * Physically reclaim posting leaves that earlier ambulkdelete passes
     * unlinked and marked deleted, now that their deletion XID has aged
     * past the global visibility horizon.  Recycled pages are recorded in
     * the index FSM (reused by pg_tre_extend) and counted in pages_free.
     * Pages still within the horizon are reported as pages_deleted so the
     * count reflects reclaimable-but-not-yet-reclaimed space.
     */
    recycled = pg_tre_posting_recycle_deleted(info->index, info->heaprel,
                                              &pending);

    /*
     * Report index-size statistics.  num_pages is the physical page
     * count; pages_free is what we just handed to the FSM (immediately
     * reusable); pages_deleted is deleted-but-not-yet-recyclable (a
     * future VACUUM will reclaim them once snapshots release).
     */
    stats->num_pages = RelationGetNumberOfBlocks(info->index);
    stats->pages_free += recycled;
    stats->pages_deleted += pending;

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
