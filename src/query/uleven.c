/*
 * src/query/uleven.c - universal Levenshtein expansion for small k.
 *
 * Given a trigram t and local edit budget k, enumerate all trigrams
 * t' with edit distance <= k (Mihov-Schulz universal Levenshtein
 * automaton).  Phase 5 uses this to OR posting lists of near-neighbor
 * trigrams when the regex is nearly literal.
 */

#include "postgres.h"

#include "pg_tre/pg_tre.h"

/* Phase 5 bodies land here. */
