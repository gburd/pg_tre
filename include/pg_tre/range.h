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
 * Iterator callback type for range bulkload.  Returns false when done.
 * Used to scan the upper tree and extract trigrams for range building.
 */
typedef bool (*UpperTrigramIterator)(void *ctx,
                                     uint64 *trigram_hash_out,
                                     BlockNumber *posting_root_out,
                                     const uint8 **inline_data_out,
                                     Size *inline_bytes_out);

/*
 * Bulk-load the range tree from the upper tree.  Scans all trigrams,
 * groups TIDs by heap block range, unions each range's trigrams into
 * a bloom filter, and writes a single range leaf page.
 *
 * Phase 5: single-leaf implementation.  Returns the range leaf's block
 * number, or InvalidBlockNumber if no ranges were built.
 */
extern BlockNumber pg_tre_range_bulkload(Relation index,
                                         UpperTrigramIterator iter,
                                         void *iter_ctx);

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

/*
 * Rebuild the range tree from scratch.  Used by pending-list merge
 * after the upper tree is rebuilt.  Phase 5 stub; Phase 8 optimizes.
 */
extern void pg_tre_range_rebuild(Relation index);

#endif /* PG_TRE_RANGE_H */
