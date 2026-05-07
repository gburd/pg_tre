/*
 * src/am/amvacuum.c - VACUUM hooks.
 *
 * Phase 4 implements both bulk-delete (scan posting trees, remove dead
 * TIDs) and cleanup (merge pending list into posting trees, compact).
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "commands/vacuum.h"
#include "utils/elog.h"

#include "pg_tre/amapi.h"

IndexBulkDeleteResult *
pg_tre_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                    IndexBulkDeleteCallback callback, void *callback_state)
{
    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

    /*
     * Phase 0 stub: index is always empty in this phase, so a no-op
     * bulkdelete is correct (not wrong, just does no work).
     */
    return stats;
}

IndexBulkDeleteResult *
pg_tre_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
    return stats;
}
