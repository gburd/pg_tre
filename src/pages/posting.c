/*
 * src/pages/posting.c - posting-tree read/write implementation.
 *
 * CONTRACT FILE. Agent A owns this; Agent B (Phase 3) needs stubs to link.
 * Phase 1/2 bodies land here when Agent A completes ambuild.
 *
 * For now: scaffolding stubs that return empty results.
 */

#include "postgres.h"

#include "pg_tre/posting.h"
#include "pg_tre/sparsemap.h"
#include "utils/rel.h"

#include <string.h>

/* ---- Reader side stubs ---- */

struct PgTrePostingScan
{
	int placeholder;
};

PgTrePostingScan *
pg_tre_posting_scan_begin(Relation index, BlockNumber root,
						  const uint8 *inline_data, Size inline_bytes)
{
	elog(ERROR, "pg_tre_posting_scan_begin: Phase 2 not yet landed");
	return NULL;
}

bool
pg_tre_posting_scan_next(PgTrePostingScan *s, sparsemap_t **out,
						 BlockNumber *min_tid_blk, BlockNumber *max_tid_blk)
{
	elog(ERROR, "pg_tre_posting_scan_next: Phase 2 not yet landed");
	return false;
}

sparsemap_t *
pg_tre_posting_materialize(Relation index, BlockNumber root,
						   const uint8 *inline_data, Size inline_bytes)
{
	/* Return an empty sparsemap for now */
	return sparsemap(4096);
}

void
pg_tre_posting_scan_end(PgTrePostingScan *s)
{
	if (s)
		pfree(s);
}
