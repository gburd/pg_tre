/*
 * include/pg_tre/uleven.h - universal Levenshtein expansion API.
 *
 * Phase 5: enumerate trigrams within edit distance k for query expansion.
 */

#ifndef PG_TRE_ULEVEN_H
#define PG_TRE_ULEVEN_H

#include "postgres.h"

/*
 * Expand a trigram to include all trigrams within edit distance k.
 * Writes up to max_out distinct trigrams to the `out` array.
 * Returns the number of trigrams written, or -1 on overflow.
 *
 * - k=0: returns 1 (the original trigram only)
 * - k=1: returns ~hundreds (substitutions, insertions, deletions)
 * - k=2: returns ~thousands (nested expansion, deduped)
 * - k>2: returns -1 (not supported; fanout explosion)
 */
extern int pg_tre_uleven_expand(const uint8 tri[3], int k,
                                uint8 (*out)[3], int max_out);

#endif /* PG_TRE_ULEVEN_H */
