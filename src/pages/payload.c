/*
 * src/pages/payload.c - per-posting payload region (positions + per-tuple bloom).
 *
 * Phase 5: accessor by sparsemap rank; inline variable-length position
 * lists and fixed-size tuple bloom signatures.  Phase 3 leaves this
 * code inert (payload not yet written).
 */

#include "postgres.h"

#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"

/* Phase 5 bodies land here. */
