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
 * Payload (legacy, 3.0.0): pre-3.0 leaves could carry, parallel to the
 * sparsemap, a per-TID payload region with (position_list,
 * tuple_bloom_128) feeding a lossy positional pre-filter.  That WRITE
 * path was removed in 3.0.0 -- new leaves are payload-free
 * (payload_bytes == 0).  OLD payload-bearing leaves are still READ
 * tolerantly (vacuum repack walks them correctly) but the payload is
 * never consulted; the executor recheck is authoritative.
 */

#ifndef PG_TRE_POSTING_H
#define PG_TRE_POSTING_H

#include "postgres.h"
#include "access/genam.h"
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
 * Opaque builder state.  Accumulates TIDs for a single trigram in
 * memory; flushed to one or more posting-tree leaves when the in-memory
 * sparsemap serialization approaches the leaf budget.
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

/* Append one TID.  The positions / n_positions / tuple_bloom_bits
 * arguments are accepted for ABI compatibility but ignored: the
 * per-tuple payload write path was removed in 3.0.0 (new leaves are
 * payload-free).  Pass NULL / 0. */
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

/*
 * Coalescing-aware finish (v2.0).  Identical to
 * pg_tre_posting_build_finish, but when `cw` is non-NULL and the
 * serialized posting falls in the medium bucket
 * (PG_TRE_INLINE_POSTING_MAX < size <= PG_TRE_COALESCE_MAX), the
 * sparsemap is packed into a shared coalesced page via `cw` instead of
 * getting a dedicated single-leaf posting tree.  In that case the
 * function returns InvalidBlockNumber, sets *inline_data_out = NULL,
 * *inline_bytes_out = 0, sets *coalesced_out = true, and writes the
 * coalesced page block + slot to *cblk_out / *cslot_out.  For all other
 * size buckets (and when cw is NULL) it behaves exactly like
 * pg_tre_posting_build_finish and sets *coalesced_out = false.
 *
 * Coalescing is off by default (pg_tre.coalesce_enable).
 */
struct PgTreCoalescedWriter;
extern BlockNumber pg_tre_posting_build_finish_ex(PgTrePostingBuilder *b,
                                                  const uint8 **inline_data_out,
                                                  Size *inline_bytes_out,
                                                  struct PgTreCoalescedWriter *cw,
                                                  bool *coalesced_out,
                                                  BlockNumber *cblk_out,
                                                  uint16 *cslot_out);

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
                                     sm_t **out,
                                     BlockNumber *min_tid_blk,
                                     BlockNumber *max_tid_blk);

/*
 * Convenience: materialize the entire posting into a newly allocated
 * sparsemap under the caller's memory context.  Suitable for trigrams
 * whose posting is expected to be small or already cached hot.
 */
extern sm_t *pg_tre_posting_materialize(Relation index,
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
    bool         owns_inline;     /* true => inline_data is a palloc'd copy
                                   * (coalesced slot) owned by this ref;
                                   * pg_tre_upper_release pfrees it */
} PgTreUpperRef;

extern bool pg_tre_upper_lookup(Relation index, uint64 trigram_hash,
                                PgTreUpperRef *out);
/*
 * Root-parameterized lookup (Phase B1.2): resolve a trigram against
 * an arbitrary upper-tree root, used by the multi-run scan to look up
 * a trigram within a specific run.
 */
extern bool pg_tre_upper_lookup_root(Relation index, BlockNumber root_upper,
                                     uint64 trigram_hash, PgTreUpperRef *out);
extern void pg_tre_upper_release(PgTreUpperRef *ref);

/*
 * Total distinct-TID cardinality of a posting tree, for planner
 * selectivity (Phase A / A3).  `cap > 0` bounds plan-time I/O: once
 * the running total reaches `cap` the walk stops and returns `cap`.
 */
extern uint64 pg_tre_posting_cardinality(Relation index, BlockNumber root,
                                         const uint8 *inline_data,
                                         Size inline_bytes, uint64 cap);

/* ---- Bulk delete (VACUUM, issue C2) ---- */

/*
 * Walk every posting tree reachable from the upper tree and strip the
 * heap TIDs that `callback` reports dead, repacking each affected leaf
 * in place and WAL-logging the change (XLOG_PTRE_VACUUM full-page
 * image).  Surviving TIDs are preserved.
 *
 * Emptied non-head leaves are unlinked from their right-link chain and
 * marked deleted (XLOG_PTRE_POSTING_UNLINK); they are physically
 * reclaimed into the index FSM by a later call to
 * pg_tre_posting_recycle_deleted() once safe (deferred-recycle).
 *
 * Returns the number of TIDs removed.  *out_remaining (optional)
 * receives the count of surviving TIDs across all reachable posting
 * trees (inline and out-of-line); *out_pages (optional) receives the
 * number of posting leaf pages visited; *out_deleted (optional) receives
 * the number of leaves unlinked this pass.
 *
 * Inline postings (stored in the upper-tree leaf entry) are repacked in
 * place; their dead TIDs are removed and counted into *out_remaining.
 */
extern uint64 pg_tre_posting_bulk_delete(Relation index,
                                         IndexBulkDeleteCallback callback,
                                         void *callback_state,
                                         uint64 *out_remaining,
                                         BlockNumber *out_pages,
                                         BlockNumber *out_deleted);

/*
 * Physically reclaim posting leaves previously unlinked + marked deleted
 * by pg_tre_posting_bulk_delete, once their deletion XID is old enough
 * that no snapshot could still be traversing the pre-unlink chain
 * (nbtree-style XID-gated recycle).  Re-initializes each reclaimable page
 * (XLOG_PTRE_POSTING_RECYCLE) and records it free in the index FSM.
 *
 * `heaprel` is the heap relation the index belongs to (IndexVacuumInfo
 * .heaprel); it is the relation passed to GlobalVisCheckRemovableFullXid
 * to compute the removable horizon, exactly as nbtree's
 * _bt_pendingfsm_finalize does.
 *
 * Returns the number of pages recycled; *out_pending (optional) receives
 * the count of deleted pages not yet recyclable (still within the
 * visibility horizon -- a future VACUUM will reclaim them).  Call from
 * amvacuumcleanup.
 */
extern BlockNumber pg_tre_posting_recycle_deleted(Relation index,
                                                  Relation heaprel,
                                                  BlockNumber *out_pending);

#endif /* PG_TRE_POSTING_H */
