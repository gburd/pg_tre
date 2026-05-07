/*
 * src/am/amscan.c - scan path.
 *
 * Phase 3 implements the exact-regex scan, Phase 5 extends it with the
 * three-tier filter (range bloom, per-tuple bloom, positional intersect).
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "nodes/tidbitmap.h"
#include "utils/elog.h"
#include "utils/rel.h"

#include "pg_tre/amapi.h"

IndexScanDesc
pg_tre_ambeginscan(Relation index, int nkeys, int norderbys)
{
    IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
    scan->opaque = NULL;       /* Phase 3 attaches scan state */
    return scan;
}

void
pg_tre_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                ScanKey orderbys, int norderbys)
{
    if (keys && scan->numberOfKeys > 0)
        memcpy(scan->keyData, keys,
               scan->numberOfKeys * sizeof(ScanKeyData));
}

int64
pg_tre_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("pg_tre amgetbitmap not yet implemented"),
             errhint("Tracked by Phase 3 (exact regex) and Phase 5 "
                     "(approximate regex) of the implementation plan.")));
    return 0;
}

void
pg_tre_amendscan(IndexScanDesc scan)
{
    /* No state attached yet. */
}
