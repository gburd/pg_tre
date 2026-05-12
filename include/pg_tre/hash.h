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

#endif /* PG_TRE_HASH_H */
