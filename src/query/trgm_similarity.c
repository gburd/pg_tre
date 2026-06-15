/*
 * src/query/trgm_similarity.c - pg_trgm-compatible trigram-set
 * similarity (Phase A, goal A2).
 *
 * NORTH STAR: pg_tre is a REGEX index with edit distance.  These
 * functions exist only so a user can drop pg_trgm and keep its
 * cheap, stateless similarity operators (%, <->, word_similarity)
 * with the SAME numeric semantics.  They are deliberately a
 * separate, self-contained trigram model from pg_tre's internal
 * codepoint trigrams: to match pg_trgm's similarity() numerically
 * we must replicate pg_trgm's exact tokenization, which differs
 * from the index's model.
 *
 * pg_trgm model (verified against the live extension):
 *   - lowercase the input,
 *   - split into "words" on non-alphanumeric boundaries,
 *   - pad each word with 2 leading spaces and 1 trailing space,
 *   - the trigrams are the 3-character windows of each padded word,
 *   - the trigram SET is deduplicated,
 *   - similarity = |A intersect B| / |A union B|  (Jaccard),
 *   - distance (<->) = 1 - similarity.
 *
 * We operate on UTF-8 codepoints (matching pg_trgm's multibyte
 * handling) and hash each 3-codepoint window to a uint32 for set
 * operations; collisions are astronomically unlikely to perturb a
 * Jaccard ratio and pg_trgm itself hashes multibyte trigrams.
 */

#include "postgres.h"

#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "varatt.h"
#include "utils/builtins.h"

#include "pg_tre/hash.h"
#include "pg_tre/pg_tre.h"

/* Sorted, deduplicated array of trigram hashes for one string. */
typedef struct TrgmSet
{
    uint32     *hashes;
    int         n;
} TrgmSet;

static int
cmp_u32(const void *a, const void *b)
{
    uint32 x = *(const uint32 *) a;
    uint32 y = *(const uint32 *) b;
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

/*
 * Build the deduplicated trigram-hash set for `str` using
 * pg_trgm's tokenization.  Returns a palloc'd TrgmSet; caller
 * pfrees set.hashes.
 */
static TrgmSet
trgm_set(const char *str, int len)
{
    /*
     * Decode to lowercase codepoints, tracking word boundaries.
     * For each maximal run of alphanumeric codepoints (a "word"),
     * emit trigrams over [sp, sp, c0, c1, ..., cN, sp] where sp is
     * a space codepoint -- i.e. 2 leading + 1 trailing space, as
     * pg_trgm does.
     */
    int32      *cps;           /* lowercased codepoints of the whole string */
    bool       *isword;        /* alnum flag per codepoint */
    int         ncp = 0;
    int         cap = len + 1;
    const char *p = str;
    const char *end = str + len;
    TrgmSet     out;
    uint32     *hbuf;
    int         hn = 0;
    int         hcap;

    cps = (int32 *) palloc(sizeof(int32) * cap);
    isword = (bool *) palloc(sizeof(bool) * cap);

    while (p < end)
    {
        int         clen = pg_mblen(p);
        pg_wchar    wc;

        if (clen <= 0 || p + clen > end)
            clen = 1;
        (void) pg_mb2wchar_with_len(p, &wc, clen);

        /*
         * Lowercase ASCII A-Z; leave other codepoints as-is
         * (pg_trgm uses locale lowercasing, but for the common
         * ASCII case this matches; non-ASCII letters still form
         * trigrams, just not case-folded -- documented limitation).
         */
        if (wc >= 'A' && wc <= 'Z')
            wc += ('a' - 'A');

        if (ncp >= cap)
        {
            cap *= 2;
            cps = (int32 *) repalloc(cps, sizeof(int32) * cap);
            isword = (bool *) repalloc(isword, sizeof(bool) * cap);
        }
        cps[ncp] = (int32) wc;
        /* alphanumeric test: ASCII alnum, or any codepoint >= 128
         * (treat all multibyte as word chars, as pg_trgm largely
         * does for CJK/accented text). */
        isword[ncp] = (wc >= 128) ||
                      (wc >= '0' && wc <= '9') ||
                      (wc >= 'a' && wc <= 'z') ||
                      (wc >= 'A' && wc <= 'Z');
        ncp++;
        p += clen;
    }

    /* Upper bound on trigram count: ~ncp + 2 per word; be generous. */
    hcap = ncp + 8;
    hbuf = (uint32 *) palloc(sizeof(uint32) * hcap);

    {
        int i = 0;
        while (i < ncp)
        {
            int wstart, wend;
            int32 ring[3];

            /* skip non-word codepoints */
            while (i < ncp && !isword[i])
                i++;
            if (i >= ncp)
                break;
            wstart = i;
            while (i < ncp && isword[i])
                i++;
            wend = i;   /* [wstart, wend) is one word */

            /*
             * Emit trigrams over [SP, SP, word..., SP].  Maintain a
             * 3-codepoint ring; first two are spaces.
             */
            {
                int32 padded_first = ' ';
                int wi;
                ring[0] = ' ';
                ring[1] = ' ';
                /* the sequence of "characters" to slide over */
                /* positions: sp sp w0 w1 ... w(k-1) sp */
                /* We feed w0..w(k-1) then a trailing space. */
                (void) padded_first;
                for (wi = wstart; wi <= wend; wi++)
                {
                    int32 ch = (wi == wend) ? ' ' : cps[wi];
                    int32 t[3];
                    ring[2] = ch;
                    t[0] = ring[0];
                    t[1] = ring[1];
                    t[2] = ring[2];
                    if (hn >= hcap)
                    {
                        hcap *= 2;
                        hbuf = (uint32 *) repalloc(hbuf, sizeof(uint32) * hcap);
                    }
                    hbuf[hn++] = (uint32) pg_tre_hash_trigram_cp(t);
                    ring[0] = ring[1];
                    ring[1] = ring[2];
                }
            }
        }
    }

    pfree(cps);
    pfree(isword);

    /* dedup: sort + unique */
    if (hn > 1)
        qsort(hbuf, hn, sizeof(uint32), cmp_u32);
    {
        int w = 0, r;
        for (r = 0; r < hn; r++)
        {
            if (w == 0 || hbuf[r] != hbuf[w - 1])
                hbuf[w++] = hbuf[r];
        }
        hn = w;
    }

    out.hashes = hbuf;
    out.n = hn;
    return out;
}

/* |A intersect B| for two sorted, deduplicated sets. */
static int
set_intersect_count(const TrgmSet *a, const TrgmSet *b)
{
    int i = 0, j = 0, c = 0;
    while (i < a->n && j < b->n)
    {
        if (a->hashes[i] == b->hashes[j])
        {
            c++; i++; j++;
        }
        else if (a->hashes[i] < b->hashes[j])
            i++;
        else
            j++;
    }
    return c;
}

static float4
trgm_similarity_impl(text *ta, text *tb)
{
    TrgmSet a = trgm_set(VARDATA_ANY(ta), VARSIZE_ANY_EXHDR(ta));
    TrgmSet b = trgm_set(VARDATA_ANY(tb), VARSIZE_ANY_EXHDR(tb));
    int inter, uni;
    float4 sim;

    if (a.n == 0 && b.n == 0)
    {
        pfree(a.hashes);
        pfree(b.hashes);
        return 0.0f;
    }
    inter = set_intersect_count(&a, &b);
    uni = a.n + b.n - inter;
    sim = (uni == 0) ? 0.0f : (float4) inter / (float4) uni;
    pfree(a.hashes);
    pfree(b.hashes);
    return sim;
}

PG_FUNCTION_INFO_V1(pg_tre_trgm_similarity);
Datum
pg_tre_trgm_similarity(PG_FUNCTION_ARGS)
{
    text *a = PG_GETARG_TEXT_PP(0);
    text *b = PG_GETARG_TEXT_PP(1);
    PG_RETURN_FLOAT4(trgm_similarity_impl(a, b));
}

PG_FUNCTION_INFO_V1(pg_tre_trgm_distance);
Datum
pg_tre_trgm_distance(PG_FUNCTION_ARGS)
{
    text *a = PG_GETARG_TEXT_PP(0);
    text *b = PG_GETARG_TEXT_PP(1);
    PG_RETURN_FLOAT4(1.0f - trgm_similarity_impl(a, b));
}

/*
 * `text % text` -> bool: similarity >= pg_tre.similarity_threshold.
 */
PG_FUNCTION_INFO_V1(pg_tre_trgm_sim_op);
Datum
pg_tre_trgm_sim_op(PG_FUNCTION_ARGS)
{
    text *a = PG_GETARG_TEXT_PP(0);
    text *b = PG_GETARG_TEXT_PP(1);
    float4 sim = trgm_similarity_impl(a, b);
    PG_RETURN_BOOL(sim >= pg_tre_similarity_threshold);
}
