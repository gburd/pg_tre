/*
 * src/util/bloom.c - fixed-size bloom filter implementation.
 *
 * Uses double-hashing: for hash functions 0..k-1, the i'th bit position
 * is computed as (h1 + i*h2) % m_bits, where h1 and h2 are the low and
 * high 32 bits of the 64-bit trigram hash.
 *
 * This avoids needing k independent hash functions and matches the
 * approach in "Less Hashing, Same Performance: Building a Better Bloom
 * Filter" (Kirsch & Mitzenmacher, 2006).
 */

#include "postgres.h"

#include <string.h>

#include "pg_tre/bloom.h"
#include "pg_tre/pg_tre.h"

Size
pg_tre_bloom_size_bytes(uint16 m_bits)
{
    Size header_size = MAXALIGN(sizeof(PgTreBloom));
    Size bits_size = MAXALIGN((m_bits + 7) / 8);
    return header_size + bits_size;
}

/*
 * Variable-width bloom helpers (format v5).  See bloom.h for design.
 *
 * Width table: code -> bits.  Designed for natural-language
 * tuples whose distinct-trigram count is roughly:
 *   - very short rows: <= 8 trigrams (a few words)         ->  32 bits
 *   - short:           9-24 trigrams (a sentence)          ->  64 bits
 *   - typical:        25-64 trigrams (a paragraph header)  -> 128 bits
 *   - long:           65-200 trigrams (a paragraph)        -> 256 bits
 *   - very long:     >200 trigrams (a document field)      -> 512 bits
 * Cap is the index-wide GUC pg_tre.bloom_tuple_bits.
 */
static const uint16 pg_tre_bloom_m_table[PG_TRE_BLOOM_M_NCODES] = {
    32, 64, 128, 256, 512,
};

uint16
pg_tre_bloom_m_for_code(uint8 m_code)
{
    if (m_code >= PG_TRE_BLOOM_M_NCODES)
        return pg_tre_bloom_m_table[PG_TRE_BLOOM_M_NCODES - 1];
    return pg_tre_bloom_m_table[m_code];
}

uint8
pg_tre_bloom_select_m_code(uint32 n_distinct, uint16 cap_bits)
{
    uint8 i;
    uint8 want;

    if      (n_distinct <=   8) want = 0;     /*  32 bits */
    else if (n_distinct <=  24) want = 1;     /*  64 bits */
    else if (n_distinct <=  64) want = 2;     /* 128 bits */
    else if (n_distinct <= 200) want = 3;     /* 256 bits */
    else                        want = 4;     /* 512 bits */

    /* Honor the cap by stepping down to the largest code whose
     * m fits within cap_bits. */
    while (want > 0 && pg_tre_bloom_m_table[want] > cap_bits)
        want--;
    /* If even the smallest preset exceeds the cap, fall through
     * with want=0; pg_tre_bloom_m_for_code will return 32, which
     * we accept rather than rejecting -- the cap is advisory. */
    (void) i;
    return want;
}

uint8
pg_tre_bloom_select_k(uint16 m, uint32 n_distinct)
{
    /*
     * Optimal k for target FPR with n elements in m bits is
     *   k = (m / n) * ln 2 ~= (m * 693 / 1000) / n
     * (uint32 arithmetic; ln(2) approximated as 0.693).  Floor
     * to >= 1 and cap at 16 to keep the inner loop bounded.
     */
    uint32 k;

    if (n_distinct == 0)
        n_distinct = 1;
    k = ((uint32) m * 693U) / (1000U * n_distinct);
    if (k < 1) k = 1;
    if (k > 16) k = 16;
    return (uint8) k;
}

Size
pg_tre_bloom_v5_wire_bytes(uint16 m_bits)
{
    /* 2-byte header [m_code:1][k:1] + ceil(m/8) bit-array bytes,
     * MAXALIGN'd so packed-adjacent blooms keep their headers
     * aligned for u16 reads. */
    return MAXALIGN(2 + (m_bits + 7) / 8);
}

void
pg_tre_bloom_init(PgTreBloom *b, uint16 m_bits, uint8 k)
{
    Size bits_bytes;

    Assert(b != NULL);
    Assert(m_bits > 0);
    Assert(k > 0);

    b->m_bits = m_bits;
    b->k = k;
    b->_pad = 0;

    bits_bytes = (m_bits + 7) / 8;
    memset(pg_tre_bloom_bits(b), 0, bits_bytes);
}

/*
 * Set bit at position 'pos' in the bloom filter.
 */
static inline void
bloom_set_bit(PgTreBloom *b, uint32 pos)
{
    uint8 *bits = pg_tre_bloom_bits(b);
    uint32 byte_idx = pos / 8;
    uint32 bit_idx = pos % 8;

    Assert(pos < b->m_bits);
    bits[byte_idx] |= (1 << bit_idx);
}

/*
 * Test if bit at position 'pos' is set.
 */
static inline bool
bloom_test_bit(const PgTreBloom *b, uint32 pos)
{
    const uint8 *bits = pg_tre_bloom_bits_const(b);
    uint32 byte_idx = pos / 8;
    uint32 bit_idx = pos % 8;

    Assert(pos < b->m_bits);
    return (bits[byte_idx] & (1 << bit_idx)) != 0;
}

void
pg_tre_bloom_add_trigram(PgTreBloom *b, uint64 trigram_hash)
{
    uint32 h1 = (uint32) (trigram_hash & 0xFFFFFFFF);       /* low 32 bits */
    uint32 h2 = (uint32) (trigram_hash >> 32);              /* high 32 bits */
    uint32 m = b->m_bits;
    uint8  k = b->k;
    uint8  i;

    /* Avoid h2 == 0 which would cause all positions to be h1. */
    if (h2 == 0)
        h2 = 1;

    for (i = 0; i < k; i++)
    {
        /*
         * Double-hashing formula: pos = (h1 + i*h2) % m.
         * Use 64-bit arithmetic to avoid overflow.
         */
        uint64 pos64 = ((uint64) h1 + (uint64) i * (uint64) h2) % m;
        bloom_set_bit(b, (uint32) pos64);
    }
}

bool
pg_tre_bloom_contains_trigram(const PgTreBloom *b, uint64 trigram_hash)
{
    uint32 h1 = (uint32) (trigram_hash & 0xFFFFFFFF);
    uint32 h2 = (uint32) (trigram_hash >> 32);
    uint32 m = b->m_bits;
    uint8  k = b->k;
    uint8  i;

    if (h2 == 0)
        h2 = 1;

    for (i = 0; i < k; i++)
    {
        uint64 pos64 = ((uint64) h1 + (uint64) i * (uint64) h2) % m;
        if (!bloom_test_bit(b, (uint32) pos64))
            return false;
    }
    return true;
}

void
pg_tre_bloom_union(PgTreBloom *dst, const PgTreBloom *src)
{
    uint8 *dst_bits = pg_tre_bloom_bits(dst);
    const uint8 *src_bits = pg_tre_bloom_bits_const(src);
    Size bytes = (dst->m_bits + 7) / 8;
    Size i;

    Assert(dst->m_bits == src->m_bits);
    Assert(dst->k == src->k);

    for (i = 0; i < bytes; i++)
        dst_bits[i] |= src_bits[i];
}

/*
 * Multi-version per-tuple bloom decoder.
 *
 * Today (v3, v4) the per-tuple bloom is a fixed-size bit vector and the
 * decode path is the same for both versions.  This dispatch lives here
 * so the call sites in src/am/amscan.c are version-agnostic; once a
 * later format introduces a different on-disk layout (e.g.
 * variable-width per-tuple blooms in a future v5+) the new arm can be
 * added in this single place.
 */
bool
pg_tre_bloom_decode_tuple(PgTreBloom *view_buf,
                          uint16 m_bits,
                          uint8 k,
                          uint32 page_format_version)
{
    Assert(view_buf != NULL);

    switch (page_format_version)
    {
        case 3:
        case 4:
            /*
             * Fixed-width bloom: header + (m_bits + 7)/8 byte vector.
             * The caller has already copied the bit array into the
             * MAXALIGN(sizeof(PgTreBloom)) offset; here we only have
             * to populate the header.  Avoid pg_tre_bloom_init() --
             * it zeroes the bit array, which would wipe the bytes
             * the caller just read.
             */
            view_buf->m_bits = m_bits;
            view_buf->k      = k;
            view_buf->_pad   = 0;
            return true;
        default:
            /*
             * Unknown / future format the running build cannot decode.
             * Caller should treat this as "no bloom available" and
             * fall back to recheck rather than ereport().
             */
            return false;
    }
}
