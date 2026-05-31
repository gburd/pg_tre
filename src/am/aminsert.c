/*
 * src/am/aminsert.c - per-tuple index insert.
 *
 * Phase 4: tokenize the new value into byte trigrams, append
 * (trigram_hash, TID, position) triples to the fast-update pending
 * list.  A background merge or explicit VACUUM consumes the list.
 */

#include "postgres.h"

#include <string.h>

#include "varatt.h"

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
#include "pg_tre/utf8.h"

bool
pg_tre_aminsert(Relation index, Datum *values, bool *isnull,
                ItemPointer ht_ctid, Relation heapRel,
                IndexUniqueCheck checkUnique, bool indexUnchanged,
                IndexInfo *indexInfo)
{
    text   *txt;
    char   *str;
    int     len;
    PgTreCpStream stream;
    int32   ring[3];   /* ring buffer of last 3 codepoints */
    int     ring_pos[3]; /* byte positions where each codepoint starts */
    int     ring_n = 0;
    int32   cp;
    int     cp_start;
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

    /* Initialize codepoint stream. */
    pg_tre_cpstream_init(&stream, str, len);

    /* Streaming loop: extract codepoint trigrams. */
    while (true)
    {
        cp_start = pg_tre_cpstream_pos(&stream);
        cp = pg_tre_cpstream_next(&stream);
        if (cp < 0)
            break;

        /* Shift the ring buffer. */
        if (ring_n >= 3)
        {
            ring[0] = ring[1];
            ring[1] = ring[2];
            ring[2] = cp;
            ring_pos[0] = ring_pos[1];
            ring_pos[1] = ring_pos[2];
            ring_pos[2] = cp_start;
        }
        else
        {
            ring[ring_n] = cp;
            ring_pos[ring_n] = cp_start;
            ring_n++;
        }

        /* Emit a trigram once we have 3 codepoints. */
        if (ring_n == 3)
        {
            hashes[batch_n]    = pg_tre_hash_trigram_cp(ring);
            tids[batch_n]      = *ht_ctid;
            positions[batch_n] = (uint32) ring_pos[0];
            batch_n++;

            if (batch_n == PG_TRE_PENDING_BATCH_MAX)
            {
                pg_tre_pending_append_batch(index, hashes, tids, positions,
                                            batch_n);
                batch_n = 0;
            }
        }
    }

    if (batch_n > 0)
        pg_tre_pending_append_batch(index, hashes, tids, positions, batch_n);

    return true;
}
