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
