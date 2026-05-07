/*
 * src/am/aminsert.c - per-tuple index insert.
 *
 * Phase 4 will implement: tokenize value into trigrams, append
 * (trigram_hash, TID, position) triples to the fast-update pending
 * list, bumping a background merge when full.
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "nodes/execnodes.h"
#include "utils/elog.h"

#include "pg_tre/amapi.h"

bool
pg_tre_aminsert(Relation index, Datum *values, bool *isnull,
                ItemPointer ht_ctid, Relation heapRel,
                IndexUniqueCheck checkUnique, bool indexUnchanged,
                IndexInfo *indexInfo)
{
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("pg_tre aminsert not yet implemented"),
             errhint("Tracked by Phase 4 of the implementation plan.")));
    return false;
}
