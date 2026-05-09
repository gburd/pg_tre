/*
 * include/pg_tre/hash.h - hash functions for pg_tre.
 *
 * Phase 2 uses a simple 64-bit hash for byte trigrams.  We use a
 * portable hash based on PostgreSQL's internal hash functions to
 * ensure cross-platform consistency.
 */

#ifndef PG_TRE_HASH_H
#define PG_TRE_HASH_H

#include "postgres.h"

/*
 * Hash a byte trigram (3 bytes) to a 64-bit value.  This must be
 * stable across platforms and builds for the same input.
 */
extern uint64 pg_tre_hash_trigram(const uint8 *trigram);

#endif /* PG_TRE_HASH_H */
