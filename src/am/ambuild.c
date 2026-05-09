/*
 * src/am/ambuild.c - index build phase.
 *
 * Phase 0 stub: registers an empty index and raises NOT_IMPLEMENTED on
 * actual heap scan.  Phase 2 will implement the tuplesort-backed bulk
 * build, Phase 3 wires the scan path in turn.
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "catalog/index.h"
#include "nodes/execnodes.h"
#include "utils/elog.h"

#include "pg_tre/amapi.h"
#include "pg_tre/meta.h"
#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"

IndexBuildResult *
pg_tre_ambuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
    IndexBuildResult *result;

    /*
     * Phase 0 behavior: initialize meta page, then report zero tuples
     * indexed.  Insert/scan still raise NOT_IMPLEMENTED.  Phase 2
     * replaces this with a real tuplesort-backed build.
     */
    pg_tre_build_empty(index);

    result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
    result->heap_tuples = 0;
    result->index_tuples = 0;

    ereport(NOTICE,
            (errmsg("pg_tre: index build is a stub (Phase 0)"),
             errhint("Phase 2 of the implementation plan adds real build.")));

    return result;
}

void
pg_tre_ambuildempty(Relation index)
{
    pg_tre_build_empty(index);
}
