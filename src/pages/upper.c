/*
 * src/pages/upper.c - upper tree (trigram-hash -> posting root).
 *
 * Phase 1: B-tree page format + Lehman-Yao descent.
 * Phase 2: bulk loader.
 * Phase 4: single-entry inserts from pending-list merge.
 */

#include "postgres.h"

#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"

/* Phase 1 bodies land here. */
