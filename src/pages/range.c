/*
 * src/pages/range.c - BRIN-style range summary tier.
 *
 * Phase 5 READ stub implementations.  These functions return pass-through
 * results until Phase 5 WRITE implements the real tier-1 logic.
 *
 * NOTE: Phase 5 WRITE has a more complete implementation of this file in
 * their worktree.  At merge time, their implementation should replace this
 * stub.  For now, these stubs allow Phase 5 READ to compile and test the
 * scan-side logic without waiting for Phase 5 WRITE.
 */

#include "postgres.h"

#include "storage/block.h"
#include "utils/rel.h"

#include "pg_tre/page.h"
#include "pg_tre/range.h"

/*
 * STUB: Bulk-load the range tree.  Returns InvalidBlockNumber (no range
 * tree built), so tier-1 filtering is disabled.
 */
BlockNumber
pg_tre_range_bulkload(Relation index, UpperTrigramIterator iter, void *iter_ctx)
{
    (void) index;
    (void) iter;
    (void) iter_ctx;
    
    /* STUB: no range tier yet */
    return InvalidBlockNumber;
}

/*
 * STUB: Look up the range bloom for a heap block.  Always returns false
 * (no range found), so tier-1 filtering passes all blocks.
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
 * STUB: Iterate range entries.  Never calls callback (no ranges exist).
 */
void
pg_tre_range_scan(Relation index, PgTreRangeScanCallback callback, void *ctx)
{
    (void) index;
    (void) callback;
    (void) ctx;
    
    /* STUB: no range tier yet; nothing to iterate */
}
