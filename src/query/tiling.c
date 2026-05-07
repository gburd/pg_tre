/*
 * src/query/tiling.c - Navarro pigeonhole k+1 tiling.
 *
 * For a pattern with min-length L and max edit cost k, partition the
 * pattern's required-trigram positions into k+1 disjoint tiles; at
 * least one tile must match exactly for the overall pattern to match
 * within cost k.  Emits an OR of per-tile conjunctions to the query
 * tree.
 */

#include "postgres.h"

#include "pg_tre/pg_tre.h"

/* Phase 5 bodies land here. */
