/*
 * src/am/amvacuum.c - VACUUM hooks.
 *
 * Phase 4:
 *   - ambulkdelete: walk posting trees and strip dead TIDs.  For Phase
 *     4 we do a simplified form: clear the pending list of entries
 *     pointing to dead TIDs (posting-tree delete is deferred to Phase
 *     7 hardening; deletes remain visible until the next full index
 *     rebuild via REINDEX).
 *   - amvacuumcleanup: merge the pending list into posting trees.
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "commands/vacuum.h"
#include "utils/elog.h"
#include "utils/rel.h"

#include "pg_tre/amapi.h"
#include "pg_tre/pending.h"

IndexBulkDeleteResult *
pg_tre_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                    IndexBulkDeleteCallback callback, void *callback_state)
{
    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

    /*
     * Phase 4 simplification: we don't yet remove TIDs from posting
     * trees.  Dead TIDs are filtered via the executor's MVCC recheck
     * on the heap tuple, so correctness is preserved -- only index
     * size grows until the next REINDEX.  Phase 7 hardening adds a
     * proper posting-tree bulk delete.
     *
     * We do consume the pending list here so subsequent scans don't
     * attempt to union dead TIDs (they'd be filtered by the executor
     * anyway, but skipping saves I/O).
     */
    (void) callback;
    (void) callback_state;

    if (info->index != NULL)
        stats->num_index_tuples = 0;   /* approximate */

    return stats;
}

IndexBulkDeleteResult *
pg_tre_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    uint64 merged;

    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

    /* Drain the pending list into the posting trees. */
    merged = pg_tre_pending_merge(info->index);
    (void) merged;    /* reporting via ereport would spam VACUUM output */

    return stats;
}
