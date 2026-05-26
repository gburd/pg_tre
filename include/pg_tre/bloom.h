/*
 * include/pg_tre/bloom.h - fixed-size bloom filter for tier 1 & tier 3.
 *
 * Shared by per-tuple blooms (tier 3, 128 bits default) and per-range
 * blooms (tier 1, 2048 bits default).  Uses double-hashing with two
 * 32-bit hash values derived from a single uint64 trigram hash.
 *
 * Thread safety: not thread-safe.  External synchronization required.
 */

#ifndef PG_TRE_BLOOM_H
#define PG_TRE_BLOOM_H

#include "postgres.h"

/*
 * Variable-width per-tuple bloom support (format v6).
 *
 * On v6 posting-leaf pages each per-tuple bloom is preceded by a
 * 2-byte [m_code:u8, k:u8] header giving its width and number of
 * hash positions.  Pre-v6 pages stored fixed-size blooms whose
 * width and k were taken from index-wide GUCs.
 *
 * Width codes 0..PG_TRE_BLOOM_M_NCODES-1 map to m_bits via
 * pg_tre_bloom_m_for_code; pg_tre_bloom_select_m_code picks the
 * smallest preset that contains a tuple's distinct-trigram count
 * with target FPR ~10%, capped at PG_TRE_BLOOM_MAX_M_BITS.  k is
 * tuned to (m, n) with pg_tre_bloom_select_k.
 */
#define PG_TRE_BLOOM_M_NCODES   5
#define PG_TRE_BLOOM_MAX_M_BITS 512

extern uint16 pg_tre_bloom_m_for_code(uint8 m_code);
extern uint8  pg_tre_bloom_code_for_m(uint16 m_bits);
extern uint8  pg_tre_bloom_select_m_code(uint32 n_distinct, uint16 cap_bits);
extern uint8  pg_tre_bloom_select_k(uint16 m, uint32 n_distinct);

/*
 * Variable-length bloom filter structure.  The actual bit vector follows
 * the header inline.  Total size is MAXALIGN(sizeof(PgTreBloom)) +
 * MAXALIGN((m_bits + 7) / 8).
 */
typedef struct PgTreBloom
{
    uint16  m_bits;      /* number of bits in the bloom filter */
    uint8   k;           /* number of hash functions */
    uint8   _pad;
    /* uint8 bits[] follows inline, (m_bits + 7) / 8 bytes */
} PgTreBloom;

/*
 * Compute the total size in bytes for a bloom filter with m_bits bits.
 * Includes header + bit vector, both MAXALIGN'd.
 */
extern Size pg_tre_bloom_size_bytes(uint16 m_bits);

/*
 * Initialize a bloom filter in caller-provided storage.  The buffer pointed
 * to by 'b' must have at least pg_tre_bloom_size_bytes(m_bits) bytes.
 * All bits are cleared to zero.
 */
extern void pg_tre_bloom_init(PgTreBloom *b, uint16 m_bits, uint8 k);

/*
 * Add a trigram to the bloom filter.  The trigram_hash is a uint64 computed
 * by pg_tre_hash_trigram(); we derive two 32-bit hash values from its lo
 * and hi halves for double-hashing.
 */
extern void pg_tre_bloom_add_trigram(PgTreBloom *b, uint64 trigram_hash);

/*
 * Test whether a trigram might be in the set (true positive or false positive).
 * Returns true if all k bit positions are set, false otherwise.
 */
extern bool pg_tre_bloom_contains_trigram(const PgTreBloom *b, uint64 trigram_hash);

/*
 * Union 'src' into 'dst' by OR-ing their bit vectors.  Both must have the
 * same m_bits and k; behavior is undefined if they differ.
 */
extern void pg_tre_bloom_union(PgTreBloom *dst, const PgTreBloom *src);

/*
 * Multi-version per-tuple bloom decoder dispatch.
 *
 * Reconstruct an in-memory PgTreBloom view over a serialized bit
 * array read out of a posting leaf.  The bit array must be at offset
 * MAXALIGN(sizeof(PgTreBloom)) within view_buf, so callers should
 * arrange the buffer as [header slot][bit array] before reading
 * bytes into the bit-array slot via
 * pg_tre_posting_lookup_tuple_bloom().
 *
 * page_format_version is the per-page on-disk format the bytes came
 * from; it must satisfy PG_TRE_FORMAT_VERSION_MIN <= v <=
 * PG_TRE_FORMAT_VERSION_LATEST.  v3..v5 stored fixed-width blooms
 * whose (m_bits, k) come from the index-wide configuration; v6
 * carries per-tuple (m_bits, k) inline so the page-walker extracts
 * those values from the on-disk header and passes them in here.
 *
 * Returns true on success, false if page_format_version is
 * unsupported (caller should treat that as "no bloom available" and
 * fall back to recheck).
 */
extern bool pg_tre_bloom_decode_tuple(PgTreBloom *view_buf,
                                       uint16 m_bits,
                                       uint8 k,
                                       uint32 page_format_version);

/*
 * Accessor: return a pointer to the bit vector portion of the bloom filter.
 */
static inline uint8 *
pg_tre_bloom_bits(PgTreBloom *b)
{
    return (uint8 *) b + MAXALIGN(sizeof(PgTreBloom));
}

static inline const uint8 *
pg_tre_bloom_bits_const(const PgTreBloom *b)
{
    return (const uint8 *) b + MAXALIGN(sizeof(PgTreBloom));
}

#endif /* PG_TRE_BLOOM_H */
