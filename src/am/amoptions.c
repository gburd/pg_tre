/*
 * src/am/amoptions.c - reloptions and opclass validation.
 *
 * Phase 6 adds per-index storage options (q, range_size, bloom_bits,
 * fastupdate, pending_list_limit).  Phase 0 accepts only the empty set
 * and treats amvalidate as always-true for the single opclass we ship.
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/reloptions.h"
#include "utils/rel.h"

#include "pg_tre/amapi.h"

bytea *
pg_tre_amoptions(Datum reloptions, bool validate)
{
    /* Phase 6 attaches a relopt_kind here. */
    return NULL;
}

bool
pg_tre_amvalidate(Oid opclassoid)
{
    /* Phase 6 will validate strategy/support entries. */
    return true;
}
