/*
 * src/util/hash.c - hash functions for pg_tre.
 *
 * Phase 2 uses a simple stable hash for byte trigrams.  We combine
 * the 3 bytes into a uint32 and pass through PostgreSQL's hash_uint32
 * extended to 64 bits for better distribution.
 */

#include "postgres.h"

#include "common/hashfn.h"
#include "pg_tre/hash.h"

/*
 * Hash a 3-byte trigram to a 64-bit value.  We want a stable,
 * deterministic hash that's the same across platforms.
 *
 * Strategy: combine the 3 bytes into a 32-bit value, hash it with
 * PostgreSQL's murmurhash32 (stable), then extend to 64 bits by
 * hashing the result again with a different seed.
 */
uint64
pg_tre_hash_trigram(const uint8 *trigram)
{
    uint32  input;
    uint32  h1, h2;
    uint64  result;

    /* Pack 3 bytes into a uint32 (little-endian order for consistency). */
    input = (uint32) trigram[0] |
            ((uint32) trigram[1] << 8) |
            ((uint32) trigram[2] << 16);

    /* Hash with murmurhash32 (seed=0). */
    h1 = murmurhash32(input);

    /* Hash again with a different seed to get the upper 32 bits. */
    h2 = hash_combine(h1, input);

    /* Combine into 64 bits. */
    result = ((uint64) h1) | (((uint64) h2) << 32);

    return result;
}
