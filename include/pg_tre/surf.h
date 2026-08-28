/*
 * include/pg_tre/surf.h - Succinct Range Filter over trigram keys.
 *
 * A SuRF (Zhang et al., SIGMOD 2018) answers approximate point- and
 * range-membership queries over an ordered key set with a one-sided error:
 * it may return a false positive but NEVER a false negative.  pg_tre uses
 * it as a whole-index filter over the *order-preserving* trigram key
 * (pg_tre_trigram_key_cp): a LIKE / ^-anchored / prefix pattern that pins a
 * short literal prefix maps to a contiguous trigram-key range, and the
 * SuRF lets a scan reject the whole index when no indexed trigram falls in
 * that range -- without descending the (hash-ordered) upper tree.
 *
 * This is SuRF-Base (the truncated trie surface, no suffix bits): we index
 * the full 8-byte big-endian key, so every distinct trigram is represented
 * to its full depth and point queries are exact; range queries are exact at
 * key granularity.  The encoding is LOUDS-Sparse (labels + louds/has-child
 * bitmaps) with O(1) rank/select over cached rank superblocks.
 *
 * Keys are uint64 (see pg_tre_trigram_key_cp).  Only the low 63 bits carry
 * codepoint data; we serialize all 8 bytes big-endian so byte-lexicographic
 * order equals unsigned-integer order equals trigram order.
 */

#ifndef PG_TRE_SURF_H
#define PG_TRE_SURF_H

#include "postgres.h"

/* Opaque in-memory handle (built or deserialized). */
typedef struct PgTreSurf PgTreSurf;

/*
 * Build a SuRF from a sorted, de-duplicated array of trigram keys.
 * `keys` must be ascending with no duplicates; `n` may be 0 (empty filter).
 * Allocations are made with palloc in the current memory context.
 */
extern PgTreSurf *pg_tre_surf_build(const uint64 *keys, uint32 n);

/* Free an in-memory SuRF (palloc'd; also fine to rely on context reset). */
extern void pg_tre_surf_free(PgTreSurf *s);

/*
 * Point query: could `key` be in the set?  Exact for SuRF-Base (no suffix
 * truncation), so this returns true iff the key was inserted.
 */
extern bool pg_tre_surf_may_contain(const PgTreSurf *s, uint64 key);

/*
 * Range query: does any stored key fall in the closed interval [lo, hi]?
 * No false negatives.  lo <= hi required.
 */
extern bool pg_tre_surf_range_overlaps(const PgTreSurf *s, uint64 lo, uint64 hi);

/*
 * Serialization for the on-disk SuRF page tier.  pg_tre_surf_serialized_size
 * returns the byte length; pg_tre_surf_serialize writes exactly that many
 * bytes to `dst`.  pg_tre_surf_deserialize builds an in-memory handle from a
 * serialized image (validated: rejects malformed input with ERROR).
 */
extern Size pg_tre_surf_serialized_size(const PgTreSurf *s);
extern void pg_tre_surf_serialize(const PgTreSurf *s, uint8 *dst);
extern PgTreSurf *pg_tre_surf_deserialize(const uint8 *src, Size len);

/* Introspection for tests/EXPLAIN. */
extern uint32 pg_tre_surf_n_keys(const PgTreSurf *s);
extern uint32 pg_tre_surf_n_nodes(const PgTreSurf *s);

#endif /* PG_TRE_SURF_H */
