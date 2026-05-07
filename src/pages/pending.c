/*
 * src/pages/pending.c - fast-update pending list.
 *
 * Phase 4: append-only page chain holding (trigram_hash, tid, position)
 * entries from recent inserts.  Merge sweep consumes the list and
 * updates posting trees.  Scans union the pending list into results
 * to preserve consistency pre-merge.
 */

#include "postgres.h"

#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"

/* Phase 4 bodies land here. */
