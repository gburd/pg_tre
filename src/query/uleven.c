/*
 * src/query/uleven.c - universal Levenshtein expansion for small k.
 *
 * Given a trigram t and local edit budget k, enumerate all trigrams
 * t' with edit distance <= k (Mihov-Schulz universal Levenshtein
 * automaton).  Phase 5 uses this to OR posting lists of near-neighbor
 * trigrams when the regex is nearly literal.
 *
 * Implementation notes:
 * - For k=0: return the input trigram only.
 * - For k=1: substitutions (3*255), insertions (4*256), deletions (3).
 * - For k=2: apply k=1 transformations recursively; dedupe.
 * - All expansions respect the global fanout cap to prevent DOS.
 */

#include "postgres.h"

#include <string.h>

#include "pg_tre/pg_tre.h"
#include "pg_tre/uleven.h"

/*
 * Helper: deduplicate an array of trigrams in place.  Returns the new count.
 * Simple O(n^2) dedup is fine for small arrays (hundreds of trigrams).
 */
static int
dedupe_trigrams(uint8 (*tris)[3], int n)
{
    int i, j, out = 0;

    for (i = 0; i < n; i++)
    {
        bool dup = false;
        for (j = 0; j < out; j++)
        {
            if (tris[j][0] == tris[i][0] &&
                tris[j][1] == tris[i][1] &&
                tris[j][2] == tris[i][2])
            {
                dup = true;
                break;
            }
        }
        if (!dup)
        {
            if (out != i)
            {
                tris[out][0] = tris[i][0];
                tris[out][1] = tris[i][1];
                tris[out][2] = tris[i][2];
            }
            out++;
        }
    }
    return out;
}

/*
 * Generate all trigrams within edit distance 1 of `tri`.
 * Returns the number of distinct trigrams written to `out`, up to `max_out`.
 * Returns -1 if the expansion would exceed max_out.
 */
static int
uleven_expand_k1(const uint8 tri[3], uint8 (*out)[3], int max_out)
{
    int n = 0;
    int i, c;

    /* Original trigram */
    if (n >= max_out) return -1;
    out[n][0] = tri[0];
    out[n][1] = tri[1];
    out[n][2] = tri[2];
    n++;

    /* Substitutions: 3 positions * 255 other bytes (excluding original) */
    for (i = 0; i < 3; i++)
    {
        for (c = 0; c < 256; c++)
        {
            if (c == tri[i])
                continue;  /* not a substitution */
            if (n >= max_out) return -1;
            out[n][0] = tri[0];
            out[n][1] = tri[1];
            out[n][2] = tri[2];
            out[n][i] = (uint8) c;
            n++;
        }
    }

    /* Deletions: remove one byte, shift remainder left, pad with 0 at end.
     * This produces 2-byte strings; we treat them as trigrams with trailing 0.
     * - Delete pos 0: [tri[1], tri[2], 0]
     * - Delete pos 1: [tri[0], tri[2], 0]
     * - Delete pos 2: [tri[0], tri[1], 0]
     */
    /* Delete position 0 */
    if (n >= max_out) return -1;
    out[n][0] = tri[1];
    out[n][1] = tri[2];
    out[n][2] = 0;
    n++;

    /* Delete position 1 */
    if (n >= max_out) return -1;
    out[n][0] = tri[0];
    out[n][1] = tri[2];
    out[n][2] = 0;
    n++;

    /* Delete position 2 */
    if (n >= max_out) return -1;
    out[n][0] = tri[0];
    out[n][1] = tri[1];
    out[n][2] = 0;
    n++;

    /* Insertions: add one byte at any of 4 positions (before each byte, or at end).
     * Result is a 4-byte string; we keep only the first 3 bytes as the trigram.
     * - Insert before pos 0: [c, tri[0], tri[1]]
     * - Insert before pos 1: [tri[0], c, tri[1]]
     * - Insert before pos 2: [tri[0], tri[1], c]
     * - Insert at end:       [tri[0], tri[1], tri[2]]  (no change to trigram)
     *
     * We skip the "insert at end" case since it doesn't change the trigram.
     */
    for (c = 0; c < 256; c++)
    {
        /* Insert before position 0 */
        if (n >= max_out) return -1;
        out[n][0] = (uint8) c;
        out[n][1] = tri[0];
        out[n][2] = tri[1];
        n++;

        /* Insert before position 1 */
        if (n >= max_out) return -1;
        out[n][0] = tri[0];
        out[n][1] = (uint8) c;
        out[n][2] = tri[1];
        n++;

        /* Insert before position 2 */
        if (n >= max_out) return -1;
        out[n][0] = tri[0];
        out[n][1] = tri[1];
        out[n][2] = (uint8) c;
        n++;
    }

    /* Deduplicate before returning */
    return dedupe_trigrams(out, n);
}

/*
 * Public API: expand a trigram to include all trigrams within edit distance k.
 * Returns the number of distinct trigrams written to `out`, up to `max_out`.
 * Returns -1 if the expansion would exceed max_out (overflow).
 */
int
pg_tre_uleven_expand(const uint8 tri[3], int k, uint8 (*out)[3], int max_out)
{
    uint8 temp[16384][3];  /* large enough for k=2 expansions */
    int n, i, batch;

    if (k < 0 || max_out <= 0)
        return 0;

    if (k == 0)
    {
        /* k=0: just the original trigram */
        if (max_out < 1)
            return -1;
        out[0][0] = tri[0];
        out[0][1] = tri[1];
        out[0][2] = tri[2];
        return 1;
    }

    if (k == 1)
    {
        /* k=1: direct expansion */
        return uleven_expand_k1(tri, out, max_out);
    }

    if (k == 2)
    {
        /* k=2: expand the original trigram to k=1, then expand each result to k=1.
         * Use a temporary buffer to avoid overwriting `out` during nested expansion.
         */
        int n1 = uleven_expand_k1(tri, temp, 16384);
        if (n1 < 0)
            return -1;  /* overflow in first expansion */

        n = 0;
        for (i = 0; i < n1; i++)
        {
            /* Expand temp[i] to k=1, write into temp starting at n1 + batch_start */
            batch = uleven_expand_k1(temp[i], &temp[n1], 16384 - n1);
            if (batch < 0)
                return -1;  /* overflow */
            
            /* Copy batch results into temp at a safe offset to avoid overlap */
            int j;
            for (j = 0; j < batch && (n + j) < 16384; j++)
            {
                if (n + j >= max_out)
                    return -1;
                out[n + j][0] = temp[n1 + j][0];
                out[n + j][1] = temp[n1 + j][1];
                out[n + j][2] = temp[n1 + j][2];
            }
            n += batch;
        }

        /* Deduplicate the final result */
        return dedupe_trigrams(out, n);
    }

    /* k > 2: not supported in Phase 5 initial cut (fanout explosion) */
    return -1;
}
