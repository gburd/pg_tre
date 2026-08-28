/*
 * include/pg_tre/hash.h - hash functions for pg_tre.
 *
 * Phase 2 uses a simple 64-bit hash for byte trigrams.  Phase 3.5 extends
 * to Unicode codepoint trigrams for proper multi-byte UTF-8 support.
 */

#ifndef PG_TRE_HASH_H
#define PG_TRE_HASH_H

#include "postgres.h"

/*
 * Hash a codepoint trigram (3 int32 values) to a 64-bit value.
 * This is the primary interface for Phase 3.5+.
 */
extern uint64 pg_tre_hash_trigram_cp(const int32 cp[3]);

/*
 * Hash a byte trigram (3 bytes) to a 64-bit value.
 * Legacy interface: for ASCII text, equivalent to pg_tre_hash_trigram_cp
 * with each byte treated as a codepoint.
 */
extern uint64 pg_tre_hash_trigram(const uint8 *trigram);

/*
 * Order-preserving trigram key (3.2.0, for the SuRF range filter).
 *
 * Packs a 3-codepoint trigram into a single uint64 whose unsigned
 * ordering equals the lexicographic ordering of the trigram's codepoint
 * sequence.  Each Unicode codepoint fits in 21 bits (max 0x10FFFF), so
 * three of them fit in 63 bits:
 *
 *     key = (cp0 << 42) | (cp1 << 21) | cp2
 *
 * Unlike pg_tre_hash_trigram_cp (a murmur hash that destroys order), this
 * key lets a text prefix map to a contiguous trigram-key range, which is
 * what the SuRF filter exploits for LIKE / ^-anchored prefix pruning.
 */
#define PG_TRE_CP_BITS      21
#define PG_TRE_CP_MASK      ((uint64) ((1u << PG_TRE_CP_BITS) - 1))

static inline uint64
pg_tre_trigram_key_cp(const int32 cp[3])
{
	return (((uint64) (cp[0] & PG_TRE_CP_MASK)) << (2 * PG_TRE_CP_BITS)) |
	       (((uint64) (cp[1] & PG_TRE_CP_MASK)) << (1 * PG_TRE_CP_BITS)) |
	       ((uint64) (cp[2] & PG_TRE_CP_MASK));
}

#endif /* PG_TRE_HASH_H */
