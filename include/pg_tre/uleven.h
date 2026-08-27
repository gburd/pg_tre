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

/*
 * Codepoint-alphabet (UTF-8 aware) expansion.  For an all-ASCII trigram
 * this is identical to pg_tre_uleven_expand; for a trigram containing any
 * codepoint > 0x7F it returns only the exact trigram (bounded fanout,
 * soundness preserved by the authoritative heap recheck).  This is the
 * variant the k>0 tiling spine uses so that expanded trigrams hash the
 * same way the index built them (pg_tre_hash_trigram_cp).
 */
extern int pg_tre_uleven_expand_cp(const int32 tri[3], int k,
                                   int32 (*out)[3], int max_out);

#endif /* PG_TRE_ULEVEN_H */
