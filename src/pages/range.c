/*
 * src/pages/range.c - BRIN-style range summary tier.
 *
 * Phase 5: per-block-range bloom filter storing the union of trigrams
 * in that range, maintained on VACUUM and at index build.  Scans consult
 * this tier first to eliminate ranges that can't satisfy any required
 * trigram of the query.
 */

#include "postgres.h"

#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"

/* Phase 5 bodies land here. */
