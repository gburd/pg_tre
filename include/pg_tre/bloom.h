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

/* ----------------------------------------------------------------------
 * Variable-width per-tuple bloom support (format v4).
 *
 * Each per-tuple bloom on disk is preceded by a (width_code, k) byte
 * pair.  width_code maps to an m_bits value via PG_TRE_BLOOM_WIDTH_BITS;
 * k is the hash-function count.  k == 0 (or width_code ==
 * PG_TRE_BLOOM_WIDTH_NONE) means "no bloom — always pass".
 *
 * The width tiers are sized so that the resulting bloom holds the
 * tuple's distinct trigrams at a target FPR ~= 1-10%, with k chosen
 * from k = round(m * ln(2) / n).
 * ---------------------------------------------------------------------- */

#define PG_TRE_BLOOM_WIDTH_NONE  255   /* sentinel: no bloom payload */

/*
 * Map width_code -> m_bits.  Codes 0..5 are valid widths in increasing
 * order; 255 means no bloom.  Returns 0 for unknown codes.
 */
static inline uint16
pg_tre_bloom_width_from_code(uint8 code)
{
    switch (code)
    {
        case 0: return 32;
        case 1: return 64;
        case 2: return 128;
        case 3: return 256;
        case 4: return 512;
        case 5: return 1024;
        case PG_TRE_BLOOM_WIDTH_NONE: return 0;
        default: return 0;
    }
}

/*
 * Map m_bits -> width_code.  Round up to the next supported tier.
 * 0 maps to PG_TRE_BLOOM_WIDTH_NONE.
 */
static inline uint8
pg_tre_bloom_code_from_width(uint16 m_bits)
{
    if (m_bits == 0)   return PG_TRE_BLOOM_WIDTH_NONE;
    if (m_bits <= 32)  return 0;
    if (m_bits <= 64)  return 1;
    if (m_bits <= 128) return 2;
    if (m_bits <= 256) return 3;
    if (m_bits <= 512) return 4;
    return 5;  /* 1024 (or anything larger, clamped) */
}

/*
 * Pick a bloom width given a distinct-trigram count and a GUC cap.  We
 * scale the width to keep load factor m/n in a healthy range; the
 * result is always a value pg_tre_bloom_code_from_width can encode.
 */
static inline uint16
pg_tre_bloom_select_width(uint32 n_distinct, uint16 cap_m_bits)
{
    uint16 m;
    if (n_distinct == 0)
        return 0;
    if (n_distinct <= 8)        m = 32;
    else if (n_distinct <= 24)  m = 64;
    else if (n_distinct <= 64)  m = 128;
    else if (n_distinct <= 200) m = 256;
    else if (n_distinct <= 600) m = 512;
    else                        m = 1024;
    if (cap_m_bits > 0 && m > cap_m_bits)
        m = cap_m_bits;
    return m;
}

/*
 * Pick k from (m_bits, n_distinct) targeting ~10% FPR using
 * k = round(m * ln(2) / n), scaled by 693/1000.  Clamped to [1, 16].
 * Returns 0 when m or n is zero.
 */
static inline uint8
pg_tre_bloom_select_k(uint16 m_bits, uint32 n_distinct)
{
    uint32 k;
    if (m_bits == 0 || n_distinct == 0)
        return 0;
    k = ((uint32) m_bits * 693U) / (1000U * n_distinct);
    if (k < 1) k = 1;
    if (k > 16) k = 16;
    return (uint8) k;
}

/*
 * Raw bit-array bloom add/test.  These let callers operate on a flat
 * `uint8 bits[]` buffer (sized (m_bits+7)/8) without a PgTreBloom
 * header — useful when reading a variable-width bloom out of an
 * on-disk per-tuple payload where the (m_bits, k) pair travels
 * separately in a width_code + k byte pair.
 */
extern void pg_tre_bloom_add_trigram_raw(uint8 *bits, uint16 m_bits,
                                         uint8 k, uint64 trigram_hash);
extern bool pg_tre_bloom_contains_trigram_raw(const uint8 *bits,
                                              uint16 m_bits, uint8 k,
                                              uint64 trigram_hash);

#endif /* PG_TRE_BLOOM_H */
