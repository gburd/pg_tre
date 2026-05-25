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
