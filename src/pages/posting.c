/*
 * src/pages/posting.c - per-trigram posting tree (sparsemap-backed).
 *
 * Phase 1: page layout, leaf sparsemap accessors.
 * Phase 2: bulk loader (streaming sorted (trigram,tid) pairs).
 * Phase 3: scan iterator + sparsemap_intersection / union driver.
 * Phase 4: incremental insert and delete.
 */

#include "postgres.h"

#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/sparsemap.h"

/* Phase 1 bodies land here. */
