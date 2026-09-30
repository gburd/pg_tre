/*
 * src/am/aminsert.c - per-tuple index insert.
 *
 * Tokenize the new value into codepoint trigrams and append one
 * (trigram_hash, TID) entry per distinct trigram to the fast-update
 * pending list.  VACUUM merges the list into the posting trees.
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

/* Per-row dedup set: stack-resident for ordinary rows, capped for huge ones. */
#define SEEN_STACK  512
#define SEEN_MAX    (1u << 22)      /* 32 MB; past half full, stop deduping */

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
    int     ring_n = 0;
    int32   cp;
    uint64  seen_stack[SEEN_STACK];
    uint64 *seen = seen_stack;          /* open addressing; 0 = empty */
    uint32  cap = SEEN_STACK, n_seen = 0;
    bool    seen_zero = false;
    uint64  hashes[PG_TRE_PENDING_BATCH_MAX];
    ItemPointerData tids[PG_TRE_PENDING_BATCH_MAX];
    uint32  positions[PG_TRE_PENDING_BATCH_MAX];
    int     batch_n = 0, i;

    if (isnull[0])
        return false;

    /* Phase 4 indexes only the first (text) column. */
    txt = DatumGetTextPP(values[0]);
    str = VARDATA_ANY(txt);
    len = VARSIZE_ANY_EXHDR(txt);

    /*
     * Append each distinct trigram once, as the bulk build does.  Every
     * reader of the pending list (merge, scan overlay) ignores the
     * position and collapses repeats into one posting TID, so repeats
     * were pure waste: a source file repeats its trigrams ~9x, and at 24
     * bytes an entry the list grew to ~24x the text it indexed (field
     * report 2026-09-29: a 137 GB index over 163 MB of source files).
     *
     * The set holds at most one hash per input byte at <= 50% load.  A row
     * too big for SEEN_MAX dedups until the set is half full and then
     * appends the rest as-is: still correct, just less compact.
     */
    while (cap < SEEN_MAX && cap < (uint32) len * 2)
        cap <<= 1;
    if (cap > SEEN_STACK)
        seen = (uint64 *) palloc0(sizeof(uint64) * cap);
    else
        memset(seen_stack, 0, sizeof(seen_stack));

    for (i = 0; i < PG_TRE_PENDING_BATCH_MAX; i++)
    {
        tids[i] = *ht_ctid;
        positions[i] = 0;
    }

    pg_tre_cpstream_init(&stream, str, len);
    while ((cp = pg_tre_cpstream_next(&stream)) >= 0)
    {
        uint64  h;

        if (ring_n >= 3)
        {
            ring[0] = ring[1];
            ring[1] = ring[2];
            ring[2] = cp;
        }
        else
            ring[ring_n++] = cp;
        if (ring_n < 3)
            continue;

        h = pg_tre_hash_trigram_cp(ring);
        if (h == 0)
        {
            if (seen_zero)
                continue;
            seen_zero = true;
        }
        else if (n_seen < cap / 2)
        {
            uint32  slot = (uint32) h & (cap - 1);

            while (seen[slot] != 0 && seen[slot] != h)
                slot = (slot + 1) & (cap - 1);
            if (seen[slot] == h)
                continue;
            seen[slot] = h;
            n_seen++;
        }

        hashes[batch_n++] = h;
        if (batch_n == PG_TRE_PENDING_BATCH_MAX)
        {
            pg_tre_pending_append_batch(index, hashes, tids, positions,
                                        batch_n);
            batch_n = 0;
        }
    }

    if (batch_n > 0)
        pg_tre_pending_append_batch(index, hashes, tids, positions, batch_n);
    if (seen != seen_stack)
        pfree(seen);

    return true;
}
