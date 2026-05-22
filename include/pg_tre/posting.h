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
 * payload is keyed by sm_rank so lookups are O(log N) in the
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

/* Inline threshold: posting sets smaller than this are stored
 * inline in the upper-tree leaf entry (no separate root page).
 * Value is in bytes of serialized sparsemap.
 *
 * Tuned in 1.2.1 from 256 to 384 bytes after measuring that the
 * dominant cost in pg_tre indexes was structural: one 8 KB
 * posting-tree page per distinct trigram, even for trigrams with
 * a handful of TIDs.  At 256 bytes the threshold caught only the
 * very rarest trigrams; at 384 bytes (room for ~48 TIDs after
 * sparsemap RLE) most natural-language trigrams stay inline,
 * cutting the page count for a 10K-row corpus from ~4700 to
 * ~700.  Inline data lives in the upper-tree leaf, which can
 * still split when its own page fills; the upper-tree split logic
 * already handles the overflow case.
 *
 * Trade-off: bigger inline data means bigger upper-tree leaves
 * and slightly slower upper-tree-leaf scans.  At 384 bytes a PG
 * page (8 KB) holds ~20 inline postings worst case before
 * splitting, vs ~30 at 256 bytes.  In practice the upper tree
 * remains shallow because most trigrams are inline; the slight
 * per-leaf cost is dominated by the page-count savings.
 */
#define PG_TRE_INLINE_POSTING_MAX 384

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

/* Sized variant: pre-allocate a sparsemap buffer large enough to hold
 * the expected number of TIDs without triggering the dynamic-resize
 * path.  Use this from the pending-list merge path where cardinality
 * is known up-front. */
extern PgTrePostingBuilder *pg_tre_posting_build_begin_sized(Relation index,
                                                             uint64 trigram_hash,
                                                             bool with_payload,
                                                             size_t expected_bytes);

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

/*
 * Number of TIDs accumulated by the builder so far.  Used by the
 * cardinality-aware build path (ambuild) to skip persisting
 * posting trees for trigrams below `pg_tre.min_trigram_freq`.
 * Returns 0 for a NULL or freshly-begun builder.
 */
extern int pg_tre_posting_build_n_tids(const PgTrePostingBuilder *b);

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

/* ---- Payload lookup (Phase 5 READ side integration point) ---- */

/*
 * Look up the per-tuple bloom filter for a given TID.  Returns true if
 * the TID is present in the posting and bloom data is available, false
 * otherwise.  The bloom is copied into *out_bloom (caller must allocate
 * sufficient space: pg_tre_bloom_size_bytes(pg_tre_bloom_tuple_bits)).
 */
extern bool pg_tre_posting_lookup_tuple_bloom(
    Relation index,
    BlockNumber root,
    const uint8 *inline_data,
    Size inline_bytes,
    uint64 packed_tid,
    uint8 *out_bloom,
    Size out_bloom_sz);

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

/* ---- Phase 5 payload APIs ---- */

/*
 * Look up the per-tuple bloom filter for a given TID in a posting tree.
 * Returns true if the TID is present in the posting and has an associated
 * bloom, false otherwise.  Copies the bloom bits into out_bloom (caller
 * must provide a buffer of at least out_bloom_sz bytes).
 *
 * Phase 5 WRITE has implemented this function.
 */
typedef struct PgTreBloom PgTreBloom;  /* forward decl from bloom.h */

extern bool pg_tre_posting_lookup_tuple_bloom(Relation index,
                                              BlockNumber root,
                                              const uint8 *inline_data,
                                              Size inline_bytes,
                                              uint64 packed_tid,
                                              uint8 *out_bloom,
                                              Size out_bloom_sz);

/*
 * Look up the list of positions where a trigram appears in a given TID.
 * Returns the number of positions found (0 if TID not present).  The
 * returned positions array points into internal storage and is valid
 * until the next call to this function.
 *
 * Phase 5 READ requires; Phase 5 WRITE implements (STUB for now).
 */
extern int pg_tre_posting_lookup_positions(Relation index,
                                           BlockNumber root,
                                           const uint8 *inline_data,
                                           Size inline_bytes,
                                           uint64 packed_tid,
                                           const uint32 **out_positions);

#endif /* PG_TRE_POSTING_H */
