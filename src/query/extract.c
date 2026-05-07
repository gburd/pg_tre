/*
 * src/query/extract.c - regex AST to trigram Boolean formula.
 *
 * Phase 3: exact-regex extraction (k=0 case).
 * Phase 5: approximate-regex extraction with edit budget, including
 * Navarro's (k+1)-tiling at the top level and local budget absorption
 * within `{~m}` sub-expressions.
 */

#include "postgres.h"

#include "pg_tre/pg_tre.h"

/* Phase 3/5 bodies land here. */
