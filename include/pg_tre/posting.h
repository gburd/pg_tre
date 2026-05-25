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
 * History:
 *   - 1.2.0  256 (initial conservative value).
 *   - 1.2.1  attempted 384; backed out:
 *       (a) wal_audit.sh post-crash differential returned 0
 *           rows for a 1000-row regex match; root cause was
 *           the O(N^2) bloom-registry corruption on rebuild.
 *       (b) at >=448 the multi-leaf 100K-row test returned 0
 *           rows for `Row 12[0-9][0-9][0-9]`; root cause was
 *           the single-trigram tier-3 unprunable false
 *           positive that fed an empty TID set into the
 *           merge.
 *       At >=1024 CREATE INDEX hung in an O(N^2) sparsemap
 *       chunk-walk during build.
 *   - 1.3.0  fixed all three: hash-based bloom registry, tail-
 *       chunk cursor on sparsemap, and tier-3 skip for
 *       unprunable single-trigram queries.
 *   - 1.3.1  raised to 2048.  Hard ceiling sits between 2048
 *       and 3072: at >=3072 the upper-tree internal level
 *       overflows on real corpora (10K rows / ~4700 distinct
 *       trigrams) because each oversized inline entry leaves
 *       too few entries per leaf for the single-level
 *       internal layer to address.  At 4096+ a separate scan
 *       bug also resurfaces in the cardinality test.  Lifting
 *       this further requires multi-level upper-tree internals
 *       (see `doc/specs/posting-page-coalescing.md`).
 */
#define PG_TRE_INLINE_POSTING_MAX 2048

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

/* Append one (TID, position, tuple-bloom-bits) triple.
 *
 * For format v4 (variable-width blooms), tuple_bloom_bits is the raw
 * bit array of size (m_bits+7)/8 bytes, and (m_bits, k) describe its
 * geometry; both are stored alongside the bits in the per-tuple
 * payload.  m_bits == 0 / k == 0 means "no bloom" and the slot is
 * encoded with the PG_TRE_BLOOM_WIDTH_NONE sentinel.
 */
extern void pg_tre_posting_build_add(PgTrePostingBuilder *b,
                                     ItemPointer tid,
                                     const uint32 *positions,
                                     int n_positions,
                                     const uint8 *tuple_bloom_bits,
                                     uint16 tuple_bloom_m_bits,
                                     uint8 tuple_bloom_k);

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
 * the TID is present in the posting and the slot carries a bloom
 * payload, false otherwise.
 *
 * On success, copies up to out_bloom_capacity bytes of bloom bits into
 * out_bloom_bits and writes the bloom geometry into *out_m_bits and
 * *out_k.  If *out_k is 0 the slot is the "always pass" sentinel and
 * the bit buffer carries no useful data.  Callers must size out_bloom
 * to at least (PG_TRE_MAX_BLOOM_BITS + 7) / 8 = 128 bytes.
 */
#define PG_TRE_MAX_BLOOM_BITS 1024

extern bool pg_tre_posting_lookup_tuple_bloom(
    Relation index,
    BlockNumber root,
    const uint8 *inline_data,
    Size inline_bytes,
    uint64 packed_tid,
    uint8 *out_bloom_bits,
    Size out_bloom_capacity,
    uint16 *out_m_bits,
    uint8 *out_k);

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

typedef struct PgTreBloom PgTreBloom;  /* forward decl from bloom.h */

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
