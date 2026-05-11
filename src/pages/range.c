/*
 * src/pages/range.c - BRIN-style range summary tier.
 *
 * Phase 5: per-block-range bloom filter storing the union of trigrams
 * in that range, maintained on VACUUM and at index build.  Scans consult
 * this tier first to eliminate ranges that can't satisfy any required
 * trigram of the query.
 *
 * Phase 5 READ stub implementations: These functions return "bloom says
 * true" until Phase 5 WRITE implements the real logic.
 */

#include "postgres.h"

#include "utils/memutils.h"
#include "utils/rel.h"

#include "pg_tre/bloom.h"
#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/range.h"

/*
 * STUB: Look up the range bloom filter covering a heap block.
 * Phase 5 READ requires; Phase 5 WRITE implements.
 *
 * For now, always returns false (no range found), so tier-1 filtering
 * passes all blocks through.  Phase 5 WRITE will implement the real lookup.
 */
bool
pg_tre_range_lookup(Relation index, BlockNumber heap_blk,
                    PgTreBloom **out_bloom)
{
    (void) index;
    (void) heap_blk;
    (void) out_bloom;
    
    /* STUB: no range tier yet; pass all blocks */
    return false;
}

/*
 * STUB: Iterate all range entries.
 * Phase 5 READ requires; Phase 5 WRITE implements.
 *
 * For now, never calls the callback (no ranges exist).
 */
void
pg_tre_range_scan(Relation index, PgTreRangeScanCallback callback, void *ctx)
{
    (void) index;
    (void) callback;
    (void) ctx;
    
    /* STUB: no range tier yet; nothing to iterate */
}
