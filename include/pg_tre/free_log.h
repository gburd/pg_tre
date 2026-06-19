/*
 * include/pg_tre/free_log.h - deferred page-free log (Blocker 2).
 *
 * When a VACUUM-time merge drops a run (Hanoi level-merge promotion or
 * a full collapse), the dropped run's pages -- its upper-tree internal
 * and leaf pages, its posting leaves, and any coalesced pages it owns
 * -- become unreachable from the run catalog, but a concurrent scan
 * that captured the run's root BEFORE the catalog rewrite committed may
 * still be descending those exact pages (VACUUM holds only
 * ShareUpdateExclusiveLock, which does not block AccessShareLock
 * scans).  We therefore must NOT free or reinitialize those pages
 * promptly: an in-flight scan must be able to traverse them intact.
 *
 * The free log is the deferred-recycle substrate for that case.  At
 * merge time we record each dropped page's block number together with
 * the deletion XID (ReadNextFullTransactionId()) in this log -- WITHOUT
 * touching the page itself.  A later VACUUM drains the log: an entry
 * becomes reclaimable only once its deletion XID has aged past the
 * global removable horizon (GlobalVisCheckRemovableFullXid), i.e. no
 * snapshot that could still be traversing the dropped run remains.
 * Reclamation reinitializes the block to a blank page (WAL-logged FPI)
 * and records it free in the index FSM, in the SAME critical section
 * that removes the log entry, so replay and re-run are idempotent.
 *
 * This mirrors the posting-leaf deferred recycle
 * (pg_tre_posting_recycle_deleted) but uses an out-of-band log rather
 * than stamping the page, because a dropped run's pages are still being
 * actively traversed top-to-bottom and cannot be repurposed in place.
 *
 * Format: the field free_log_head in the meta page (carved from the
 * former reserved[] tail; zero on indexes built before this feature ->
 * normalized to InvalidBlockNumber, "no free log") points at a chain of
 * PG_TRE_PAGE_FREE_LOG pages.  The whole mechanism is additive and does
 * NOT bump the on-disk format version: it adds a new page KIND and a
 * meta field, not a new decode of any existing page.  An index built
 * before this feature has free_log_head == InvalidBlockNumber and
 * behaves exactly as before (no deferred frees, no reclaim).  No
 * REINDEX.
 */
#ifndef PG_TRE_FREE_LOG_H
#define PG_TRE_FREE_LOG_H

#include "postgres.h"

#include "storage/block.h"
#include "utils/rel.h"

/*
 * Record the given blocks for deferred free.  Stamps a single deletion
 * XID (ReadNextFullTransactionId()) across all of them and appends them
 * to the free-log chain, creating the first log page on demand.  All
 * page edits (log page(s) + meta) and their WAL travel inside one
 * critical section per touched log page.  The blocks themselves are NOT
 * modified -- in-flight scans keep traversing them until the drain
 * reclaims them.  Caller must hold a lock excluding concurrent catalog
 * writers (VACUUM's ShareUpdateExclusiveLock).  A no-op when n <= 0.
 */
extern void
pg_tre_free_log_append(Relation index, const BlockNumber *blocks, int n);

/*
 * Drain the free log: reclaim every logged block whose deletion XID has
 * aged past the global removable horizon (no snapshot could still reach
 * it via a pre-rewrite run root).  Each reclaimed block is reinitialized
 * to a blank page (WAL-logged FPI) and recorded free in the index FSM,
 * in the same WAL record that removes its log entry.  `heaprel` is the
 * index's heap relation, passed to GlobalVisCheckRemovableFullXid.
 *
 * Returns the number of blocks reclaimed; *out_pending (optional)
 * receives the count of logged blocks not yet reclaimable (still within
 * the visibility horizon).  Call from amvacuumcleanup after the
 * posting-leaf recycle.  Caller holds a lock excluding concurrent
 * catalog writers.
 */
extern BlockNumber pg_tre_free_log_drain(
		Relation index, Relation heaprel, BlockNumber *out_pending);

/*
 * Collect every page reachable from a run's upper-tree root (upper
 * internal + upper leaf pages, out-of-line posting leaf chains, and
 * coalesced pages) into a palloc'd, deduplicated block-number array, so
 * the caller can hand them to pg_tre_free_log_append when the run is
 * dropped by a merge.  *out_n receives the count; the returned array is
 * palloc'd in CurrentMemoryContext (NULL when the run has no pages).
 * Read-only: takes only SHARE locks, mutates nothing.
 */
extern BlockNumber *
pg_tre_run_collect_pages(Relation index, BlockNumber root_upper, int *out_n);

#endif /* PG_TRE_FREE_LOG_H */
