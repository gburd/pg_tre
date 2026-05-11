/*
 * src/am/aminsert.c - per-tuple index insert.
 *
 * Phase 4: tokenize the new value into byte trigrams, append
 * (trigram_hash, TID, position) triples to the fast-update pending
 * list.  A background merge or explicit VACUUM consumes the list.
 */

#include "postgres.h"

#include <string.h>

#include "access/amapi.h"
#include "access/genam.h"
#include "nodes/execnodes.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/rel.h"

#include "pg_tre/amapi.h"
#include "pg_tre/hash.h"
#include "pg_tre/pending.h"
#include "pg_tre/pg_tre.h"

bool
pg_tre_aminsert(Relation index, Datum *values, bool *isnull,
                ItemPointer ht_ctid, Relation heapRel,
                IndexUniqueCheck checkUnique, bool indexUnchanged,
                IndexInfo *indexInfo)
{
    text   *txt;
    char   *str;
    int     len, i;
    uint64  hashes    [PG_TRE_PENDING_BATCH_MAX];
    ItemPointerData tids[PG_TRE_PENDING_BATCH_MAX];
    uint32  positions [PG_TRE_PENDING_BATCH_MAX];
    int     batch_n = 0;

    if (isnull[0])
        return false;

    /* Phase 4 indexes only the first (text) column. */
    txt = DatumGetTextPP(values[0]);
    str = VARDATA_ANY(txt);
    len = VARSIZE_ANY_EXHDR(txt);

    /* Tokenize to byte trigrams. */
    for (i = 0; i + 3 <= len; i++)
    {
        uint8 tri[3] = { (uint8) str[i], (uint8) str[i+1], (uint8) str[i+2] };
        hashes[batch_n]    = pg_tre_hash_trigram(tri);
        tids[batch_n]      = *ht_ctid;
        positions[batch_n] = (uint32) i;
        batch_n++;

        if (batch_n == PG_TRE_PENDING_BATCH_MAX)
        {
            pg_tre_pending_append_batch(index, hashes, tids, positions,
                                        batch_n);
            batch_n = 0;
        }
    }
    if (batch_n > 0)
        pg_tre_pending_append_batch(index, hashes, tids, positions, batch_n);

    return true;
}
