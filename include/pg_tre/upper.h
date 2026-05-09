/*
 * include/pg_tre/upper.h - upper tree API.
 *
 * Upper tree maps trigram_hash -> posting root (or inline blob).
 * B-tree-like structure with Lehman-Yao right-links.
 */

#ifndef PG_TRE_UPPER_H
#define PG_TRE_UPPER_H

#include "postgres.h"
#include "storage/block.h"
#include "utils/rel.h"

/*
 * Iterator function type for bulk-load.  Callback returns true and
 * fills *hash, *root, *inline_data, *inline_bytes for the next entry,
 * or returns false on EOF.  Entries must be sorted by hash ascending.
 */
typedef bool (*pg_tre_upper_bulkload_iter)(void *ctx, uint64 *hash,
                                           BlockNumber *root,
                                           const uint8 **inline_data,
                                           Size *inline_bytes);

/*
 * Bulk-load the upper tree from a sorted iterator.  Returns the root
 * block number of the new tree, or InvalidBlockNumber if empty.
 * Used by ambuild in Phase 2.
 */
extern BlockNumber pg_tre_upper_bulkload(Relation index,
                                         pg_tre_upper_bulkload_iter iter,
                                         void *iter_ctx);

/*
 * Insert a single entry into the upper tree.  Phase 4 implements
 * tree descent + split logic.
 */
extern void pg_tre_upper_insert(Relation index, uint64 hash,
                                BlockNumber root,
                                const uint8 *inline_data, Size inline_bytes);

#endif /* PG_TRE_UPPER_H */
