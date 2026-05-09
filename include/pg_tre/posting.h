/*
 * include/pg_tre/posting.h - posting-tree API.
 *
 * CONTRACT FILE.  Both the build/write side (src/pages/posting.c,
 * src/am/ambuild.c) and the scan/read side (src/am/amscan.c,
 * src/query/extract.c driver) code against this interface.  Changes
 * here require coordination across both sides.
 *
 * A "posting tree" is a B-tree-like structure whose leaf pages hold
 * a sparsemap of TIDs (one bit per TID present in the trigram set).
 * Each trigram has its own posting tree, rooted at a block number
 * recorded in the upper tree.  Posting trees with fewer than
 * PG_TRE_INLINE_POSTING_MAX TIDs are stored inline in the upper-tree
 * leaf entry to avoid one level of indirection for rare trigrams.
 *
 * Payload:  leaf pages optionally carry, parallel to the sparsemap, a
 * per-TID payload region with (position_list, tuple_bloom_128).  The
 * payload is keyed by sparsemap_rank so lookups are O(log N) in the
 * container hierarchy of sparsemap.  Payload is optional per index
 * (reloption); Phase 5 enables it by default for text opclasses.
 */

#ifndef PG_TRE_POSTING_H
#define PG_TRE_POSTING_H

#include "postgres.h"
#include "storage/block.h"
#include "storage/itemptr.h"
#include "storage/buf.h"
#include "utils/rel.h"

#include "pg_tre/page.h"
#include "pg_tre/sparsemap.h"

/* Inline threshold: posting sets smaller than this are stored inline
 * in the upper-tree leaf entry (no separate root page).  Value is in
 * bytes of serialized sparsemap. */
#define PG_TRE_INLINE_POSTING_MAX 256

/* ---- Writer side (owned by Agent A -- Phase 1/2) ---- */

/*
 * Opaque builder state.  Accumulates (TID, payload) pairs for a single
 * trigram in memory; flushed to one or more posting-tree leaves when
 * the in-memory sparsemap serialization approaches the leaf budget.
 */
typedef struct PgTrePostingBuilder PgTrePostingBuilder;

/*
 * Start building the posting tree for one trigram.  Allocates state
 * in the caller's current memory context.
 */
extern PgTrePostingBuilder *pg_tre_posting_build_begin(Relation index,
                                                       uint64 trigram_hash,
                                                       bool with_payload);

/* Append one (TID, position, tuple-bloom-bits) triple. */
extern void pg_tre_posting_build_add(PgTrePostingBuilder *b,
                                     ItemPointer tid,
                                     const uint32 *positions,
                                     int n_positions,
                                     const uint8 *tuple_bloom_bits);

/*
 * Finalize the build: emit all leaf pages (WAL-logged), possibly
 * build intermediate upper-tree entries for this trigram, and return
 * the root block (or InvalidBlockNumber if small enough to inline).
 * If inline, *inline_bytes_out receives the serialized sparsemap size
 * and the builder's buffer pointer is handed off via *inline_data_out
 * (caller must copy out before destroying the builder).
 */
extern BlockNumber pg_tre_posting_build_finish(PgTrePostingBuilder *b,
                                               const uint8 **inline_data_out,
                                               Size *inline_bytes_out);

extern void pg_tre_posting_build_free(PgTrePostingBuilder *b);

/* ---- Reader side (owned by Agent B -- Phase 3) ---- */

/*
 * Opaque iterator over one trigram's posting tree.  Emits sparsemaps
 * leaf-by-leaf so the scan driver can AND/OR them incrementally
 * without materializing a full TID set up-front.
 */
typedef struct PgTrePostingScan PgTrePostingScan;

/*
 * Start scanning the posting tree rooted at `root` (or an inline blob
 * of `inline_bytes` at `inline_data` if root is InvalidBlockNumber).
 */
extern PgTrePostingScan *pg_tre_posting_scan_begin(Relation index,
                                                   BlockNumber root,
                                                   const uint8 *inline_data,
                                                   Size inline_bytes);

/*
 * Pull the next leaf's sparsemap.  Returns true if a leaf was
 * produced, false on EOF.  The returned sparsemap is a zero-copy
 * wrapper over the pinned buffer contents; valid only until the next
 * call to scan_next or scan_end.
 */
extern bool pg_tre_posting_scan_next(PgTrePostingScan *s,
                                     sparsemap_t **out,
                                     BlockNumber *min_tid_blk,
                                     BlockNumber *max_tid_blk);

/*
 * Convenience: materialize the entire posting into a newly allocated
 * sparsemap under the caller's memory context.  Suitable for trigrams
 * whose posting is expected to be small or already cached hot.
 */
extern sparsemap_t *pg_tre_posting_materialize(Relation index,
                                               BlockNumber root,
                                               const uint8 *inline_data,
                                               Size inline_bytes);

extern void pg_tre_posting_scan_end(PgTrePostingScan *s);

/* ---- Upper-tree lookup (both sides) ---- */

/*
 * Resolve a trigram hash to its posting root.  Returns:
 *   - root block in *out_root and InvalidBlockNumber when inline;
 *   - inline bytes pointer + length when the posting is stored
 *     inline in the upper-tree leaf entry;
 *   - false if the trigram is not present in the index.
 *
 * The returned inline_data pointer is valid only while the upper-tree
 * buffer remains pinned (this function holds the pin until the caller
 * calls pg_tre_upper_release).
 */
typedef struct PgTreUpperRef
{
    Buffer       upper_buf;       /* pinned buffer, NULL if not present */
    BlockNumber  root;            /* InvalidBlockNumber if inline */
    const uint8 *inline_data;     /* NULL if not inline */
    Size         inline_bytes;
} PgTreUpperRef;

extern bool pg_tre_upper_lookup(Relation index, uint64 trigram_hash,
                                PgTreUpperRef *out);
extern void pg_tre_upper_release(PgTreUpperRef *ref);

#endif /* PG_TRE_POSTING_H */
