/*
 * src/util/hash.c - hash functions for pg_tre.
 *
 * Phase 2 uses a simple stable hash for byte trigrams.  Phase 3.5 extends
 * to Unicode codepoint trigrams: each trigram is now a sequence of 3
 * codepoints (int32 values) rather than 3 bytes.
 *
 * For ASCII text, byte trigrams and codepoint trigrams are equivalent
 * (each byte IS a codepoint), so no regression on existing tests.
 */

#include "postgres.h"

#include "common/hashfn.h"
#include "pg_tre/hash.h"

/*
 * Hash a 3-codepoint trigram to a 64-bit value.  We want a stable,
 * deterministic hash that's the same across platforms.
 *
 * Strategy: pack the 3 codepoints into a 96-bit value (conceptually),
 * hash it with PostgreSQL's murmurhash32 in two passes to get 64 bits.
 *
 * For Phase 3.5, we use a simple extension: hash the first two codepoints
 * to get h1, then combine with the third codepoint to get h2.
 */
uint64
pg_tre_hash_trigram_cp(const int32 cp[3])
{
    uint32  h1, h2;
    uint64  result;

    /*
     * Hash the first codepoint to seed h1.
     */
    h1 = murmurhash32((uint32) cp[0]);

    /*
     * Combine with the second codepoint.
     */
    h1 = hash_combine(h1, (uint32) cp[1]);

    /*
     * Hash again with the third codepoint to get h2 (upper 32 bits).
     */
    h2 = hash_combine(h1, (uint32) cp[2]);

    /*
     * Combine into 64 bits. We use h1 (after incorporating cp[0] and cp[1])
     * for the lower 32 bits and h2 (after incorporating cp[2]) for the
     * upper 32 bits.
     */
    result = ((uint64) h1) | (((uint64) h2) << 32);

    return result;
}

/*
 * Byte-trigram hash (legacy interface).
 * For ASCII text, this is equivalent to pg_tre_hash_trigram_cp where each
 * byte is treated as a codepoint.
 */
uint64
pg_tre_hash_trigram(const uint8 *trigram)
{
    int32 cp[3];

    cp[0] = (int32) trigram[0];
    cp[1] = (int32) trigram[1];
    cp[2] = (int32) trigram[2];

    return pg_tre_hash_trigram_cp(cp);
}
