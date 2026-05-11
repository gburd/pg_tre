/*
 * include/pg_tre/range.h - BRIN-style range summary tier API.
 *
 * Phase 5 range tier: per-block-range bloom filters that store the
 * union of all trigrams appearing in heap blocks within that range.
 * Used as tier-1 filter to eliminate entire heap regions before
 * consulting per-trigram posting trees.
 *
 * CONTRACT FILE for Phase 5 READ / WRITE coordination.
 */

#ifndef PG_TRE_RANGE_H
#define PG_TRE_RANGE_H

#include "postgres.h"
#include "storage/block.h"
#include "utils/rel.h"

typedef struct PgTreBloom PgTreBloom;  /* forward decl from bloom.h */

/*
 * Look up the range bloom filter covering the given heap block number.
 * Returns true if a range entry exists covering heap_blk, false otherwise.
 * The returned bloom is allocated in the caller's current memory context
 * (caller must pfree).
 *
 * Phase 5 READ requires; Phase 5 WRITE implements.
 */
extern bool pg_tre_range_lookup(Relation index, BlockNumber heap_blk,
                                PgTreBloom **out_bloom);

/*
 * Iterate all range entries.  Callback receives (range_start_blk,
 * range_end_blk, bloom) for each range in ascending block order.
 * Holds share lock on one range-tree page at a time.
 *
 * Phase 5 READ requires for tier-1 block-mask construction;
 * Phase 5 WRITE implements.
 */
typedef void (*PgTreRangeScanCallback)(BlockNumber range_start,
                                       BlockNumber range_end,
                                       const PgTreBloom *bloom,
                                       void *ctx);

extern void pg_tre_range_scan(Relation index,
                              PgTreRangeScanCallback callback,
                              void *ctx);

#endif /* PG_TRE_RANGE_H */
