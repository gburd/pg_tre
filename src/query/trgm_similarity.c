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
                    {
                        /*
                         * Fold the full 64-bit trigram hash into 32
                         * bits (XOR halves).  A bare (uint32) cast
                         * would keep only the low half, which encodes
                         * just (cp0, cp1) -- dropping the third
                         * codepoint and collapsing every "  X" leading
                         * trigram to one value.
                         */
                        uint64 fh = pg_tre_hash_trigram_cp(t);
                        hbuf[hn++] = (uint32) (fh ^ (fh >> 32));
                    }
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

/* ============================================================= *
 * word_similarity / strict_word_similarity (Phase A / A2).
 *
 * Faithful port of pg_trgm's calc_word_similarity +
 * iterate_word_similarity (PG 18 contrib/pg_trgm/trgm_op.c),
 * operating on pg_tre's codepoint-trigram hashes.  word_similarity
 * is asymmetric: it returns the greatest Jaccard similarity of
 * arg1's trigram set against any contiguous trigram "extent" of
 * arg2.  Strict mode forces extent bounds to fall on word
 * boundaries.
 *
 * CALCSML(count,len1,len2) = count / (len1 + len2 - count), as in
 * pg_trgm's default (DIVUNION) build.
 * ============================================================= */

/* Bound flags, matching pg_trgm's TrgmBound. */
#define TRE_BOUND_LEFT   0x01
#define TRE_BOUND_RIGHT  0x02

/*
 * Positional trigram list for one string: hashes in textual order
 * (duplicates kept), plus per-position word-bound flags.
 */
typedef struct PosTrgm
{
    uint32     *hashes;        /* textual-order trigram hashes */
    uint8      *bounds;        /* TRE_BOUND_* per position (NULL if unused) */
    int         n;
} PosTrgm;

/*
 * Build the positional trigram list for `str` using pg_trgm's
 * tokenization (2 leading + 1 trailing space per word).  When
 * `want_bounds`, the first trigram of each word is marked
 * TRE_BOUND_LEFT and the last TRE_BOUND_RIGHT.
 */
static PosTrgm
pos_trgm(const char *str, int len, bool want_bounds)
{
    int32      *cps;
    bool       *isword;
    int         ncp = 0;
    int         cap = len + 1;
    const char *p = str;
    const char *end = str + len;
    PosTrgm     out;
    uint32     *hbuf;
    uint8      *bbuf;
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
        if (wc >= 'A' && wc <= 'Z')
            wc += ('a' - 'A');
        if (ncp >= cap)
        {
            cap *= 2;
            cps = (int32 *) repalloc(cps, sizeof(int32) * cap);
            isword = (bool *) repalloc(isword, sizeof(bool) * cap);
        }
        cps[ncp] = (int32) wc;
        isword[ncp] = (wc >= 128) ||
                      (wc >= '0' && wc <= '9') ||
                      (wc >= 'a' && wc <= 'z');
        ncp++;
        p += clen;
    }

    hcap = ncp + 8;
    hbuf = (uint32 *) palloc(sizeof(uint32) * hcap);
    bbuf = want_bounds ? (uint8 *) palloc0(sizeof(uint8) * hcap) : NULL;

    {
        int i = 0;
        while (i < ncp)
        {
            int wstart, wend, word_first_h, wi;
            int32 ring[3];

            while (i < ncp && !isword[i])
                i++;
            if (i >= ncp)
                break;
            wstart = i;
            while (i < ncp && isword[i])
                i++;
            wend = i;

            word_first_h = hn;
            ring[0] = ' ';
            ring[1] = ' ';
            for (wi = wstart; wi <= wend; wi++)
            {
                int32 ch = (wi == wend) ? ' ' : cps[wi];
                int32 t[3];
                uint64 fh;

                ring[2] = ch;
                t[0] = ring[0];
                t[1] = ring[1];
                t[2] = ring[2];
                if (hn >= hcap)
                {
                    int oldcap = hcap;
                    hcap *= 2;
                    hbuf = (uint32 *) repalloc(hbuf, sizeof(uint32) * hcap);
                    if (bbuf)
                        bbuf = (uint8 *) repalloc0(bbuf,
                                                   sizeof(uint8) * oldcap,
                                                   sizeof(uint8) * hcap);
                }
                fh = pg_tre_hash_trigram_cp(t);
                hbuf[hn++] = (uint32) (fh ^ (fh >> 32));
                ring[0] = ring[1];
                ring[1] = ring[2];
            }
            if (bbuf && hn > word_first_h)
            {
                bbuf[word_first_h] |= TRE_BOUND_LEFT;
                bbuf[hn - 1] |= TRE_BOUND_RIGHT;
            }
        }
    }

    pfree(cps);
    pfree(isword);
    out.hashes = hbuf;
    out.bounds = bbuf;
    out.n = hn;
    return out;
}

/*
 * Iterative maximum-similarity search, a faithful port of pg_trgm's
 * iterate_word_similarity.  `trg2indexes[j]` maps position j in arg2
 * to a distinct-trigram id; `found[id]` is true when that trigram
 * also occurs in arg1; `ulen1` is the count of distinct arg1
 * trigrams; `bounds` (strict only) flags word edges per arg2
 * position.
 */
static float4
iterate_word_sim(const int *trg2indexes, const bool *found,
                 int ulen1, int len2, int len, bool strict,
                 const uint8 *bounds)
{
    int        *lastpos;
    int         i, ulen2 = 0, count = 0, upper = -1, lower;
    float4      smlr_cur, smlr_max = 0.0f;

    lower = strict ? 0 : -1;
    if (len <= 0)
        return 0.0f;
    lastpos = (int *) palloc(sizeof(int) * len);
    memset(lastpos, -1, sizeof(int) * len);

    for (i = 0; i < len2; i++)
    {
        int trgindex = trg2indexes[i];

        if (lower >= 0 || found[trgindex])
        {
            if (lastpos[trgindex] < 0)
            {
                ulen2++;
                if (found[trgindex])
                    count++;
            }
            lastpos[trgindex] = i;
        }

        if (strict ? (bounds[i] & TRE_BOUND_RIGHT) : found[trgindex])
        {
            int prev_lower, tmp_ulen2, tmp_lower, tmp_count;

            upper = i;
            if (lower == -1)
            {
                lower = i;
                ulen2 = 1;
            }

            smlr_cur = (float4) count / (float4) (ulen1 + ulen2 - count);

            tmp_count = count;
            tmp_ulen2 = ulen2;
            prev_lower = lower;
            for (tmp_lower = lower; tmp_lower <= upper; tmp_lower++)
            {
                float4 smlr_tmp;
                int tmp_trgindex;

                if (!strict || (bounds[tmp_lower] & TRE_BOUND_LEFT))
                {
                    smlr_tmp = (float4) tmp_count /
                               (float4) (ulen1 + tmp_ulen2 - tmp_count);
                    if (smlr_tmp > smlr_cur)
                    {
                        smlr_cur = smlr_tmp;
                        ulen2 = tmp_ulen2;
                        lower = tmp_lower;
                        count = tmp_count;
                    }
                }
                tmp_trgindex = trg2indexes[tmp_lower];
                if (lastpos[tmp_trgindex] == tmp_lower)
                {
                    tmp_ulen2--;
                    if (found[tmp_trgindex])
                        tmp_count--;
                }
            }

            smlr_max = Max(smlr_max, smlr_cur);

            for (tmp_lower = prev_lower; tmp_lower < lower; tmp_lower++)
            {
                int tmp_trgindex = trg2indexes[tmp_lower];
                if (lastpos[tmp_trgindex] == tmp_lower)
                    lastpos[tmp_trgindex] = -1;
            }
        }
    }

    pfree(lastpos);
    return smlr_max;
}

/*
 * One merged (trigram-hash, position) record for the
 * make_positional_trgm sort: arg1 trigrams carry index -1, arg2
 * trigrams carry their textual position.
 */
typedef struct MergeRec
{
    uint32      hash;
    int         index;     /* -1 for arg1, else arg2 position */
} MergeRec;

static int
cmp_merge(const void *a, const void *b)
{
    const MergeRec *p = (const MergeRec *) a;
    const MergeRec *q = (const MergeRec *) b;
    if (p->hash < q->hash)
        return -1;
    if (p->hash > q->hash)
        return 1;
    /* same hash: arg1 (-1) sorts before arg2 positions, then by position */
    if (p->index < q->index)
        return -1;
    if (p->index > q->index)
        return 1;
    return 0;
}

/*
 * calc_word_similarity port: arg1 is the search pattern, arg2 the
 * text searched for a similar word/extent.
 */
static float4
calc_word_sim(const char *str1, int slen1,
              const char *str2, int slen2, bool strict)
{
    PosTrgm     t1 = pos_trgm(str1, slen1, false);
    PosTrgm     t2 = pos_trgm(str2, slen2, strict);
    int         len1 = t1.n;
    int         len2 = t2.n;
    int         len = len1 + len2;
    MergeRec   *merge;
    int        *trg2indexes;
    bool       *found;
    int         ulen1 = 0, i, j;
    float4      result;

    if (len == 0)
    {
        pfree(t1.hashes);
        pfree(t2.hashes);
        if (t2.bounds)
            pfree(t2.bounds);
        return 0.0f;
    }

    /*
     * Distinct arg1 trigrams sort first per hash (index -1); each
     * arg2 position follows.  Sorting groups identical hashes so we
     * can assign a distinct-trigram id (j) and mark which ids occur
     * in arg1 (found[]).
     */
    merge = (MergeRec *) palloc(sizeof(MergeRec) * len);
    {
        /* arg1 needs distinct trigrams only, like generate_trgm_only
         * which produces a sorted unique array; emulate by dedup. */
        uint32 *u1 = (uint32 *) palloc(sizeof(uint32) * (len1 > 0 ? len1 : 1));
        int u1n = 0, k;

        for (k = 0; k < len1; k++)
            u1[u1n++] = t1.hashes[k];
        if (u1n > 1)
            qsort(u1, u1n, sizeof(uint32), cmp_u32);
        {
            int w = 0, r;
            for (r = 0; r < u1n; r++)
                if (w == 0 || u1[r] != u1[w - 1])
                    u1[w++] = u1[r];
            u1n = w;
        }

        i = 0;
        for (k = 0; k < u1n; k++)
        {
            merge[i].hash = u1[k];
            merge[i].index = -1;
            i++;
        }
        for (k = 0; k < len2; k++)
        {
            merge[i].hash = t2.hashes[k];
            merge[i].index = k;
            i++;
        }
        len = i;   /* = u1n + len2 (<= len1 + len2) */
        pfree(u1);
    }
    qsort(merge, len, sizeof(MergeRec), cmp_merge);

    trg2indexes = (int *) palloc(sizeof(int) * (len2 > 0 ? len2 : 1));
    found = (bool *) palloc0(sizeof(bool) * len);

    j = 0;
    for (i = 0; i < len; i++)
    {
        if (i > 0 && merge[i - 1].hash != merge[i].hash)
        {
            if (found[j])
                ulen1++;
            j++;
        }
        if (merge[i].index >= 0)
            trg2indexes[merge[i].index] = j;
        else
            found[j] = true;
    }
    if (len > 0 && found[j])
        ulen1++;

    result = iterate_word_sim(trg2indexes, found, ulen1, len2, len,
                              strict, t2.bounds);

    pfree(merge);
    pfree(trg2indexes);
    pfree(found);
    pfree(t1.hashes);
    pfree(t2.hashes);
    if (t2.bounds)
        pfree(t2.bounds);
    return result;
}

PG_FUNCTION_INFO_V1(pg_tre_word_similarity);
Datum
pg_tre_word_similarity(PG_FUNCTION_ARGS)
{
    text *a = PG_GETARG_TEXT_PP(0);
    text *b = PG_GETARG_TEXT_PP(1);
    PG_RETURN_FLOAT4(calc_word_sim(VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a),
                                   VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b), false));
}

PG_FUNCTION_INFO_V1(pg_tre_strict_word_similarity);
Datum
pg_tre_strict_word_similarity(PG_FUNCTION_ARGS)
{
    text *a = PG_GETARG_TEXT_PP(0);
    text *b = PG_GETARG_TEXT_PP(1);
    PG_RETURN_FLOAT4(calc_word_sim(VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a),
                                   VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b), true));
}

/* `text <% text` -> bool: word_similarity(a,b) >= threshold. */
PG_FUNCTION_INFO_V1(pg_tre_word_sim_op);
Datum
pg_tre_word_sim_op(PG_FUNCTION_ARGS)
{
    text *a = PG_GETARG_TEXT_PP(0);
    text *b = PG_GETARG_TEXT_PP(1);
    float4 s = calc_word_sim(VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a),
                             VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b), false);
    PG_RETURN_BOOL(s >= pg_tre_similarity_threshold);
}

/* `text <<-> text` -> float4: 1 - word_similarity(a,b) (distance). */
PG_FUNCTION_INFO_V1(pg_tre_word_dist_op);
Datum
pg_tre_word_dist_op(PG_FUNCTION_ARGS)
{
    text *a = PG_GETARG_TEXT_PP(0);
    text *b = PG_GETARG_TEXT_PP(1);
    float4 s = calc_word_sim(VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a),
                             VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b), false);
    PG_RETURN_FLOAT4(1.0f - s);
}

/* `text <<% text` -> bool: strict_word_similarity(a,b) >= threshold. */
PG_FUNCTION_INFO_V1(pg_tre_strict_word_sim_op);
Datum
pg_tre_strict_word_sim_op(PG_FUNCTION_ARGS)
{
    text *a = PG_GETARG_TEXT_PP(0);
    text *b = PG_GETARG_TEXT_PP(1);
    float4 s = calc_word_sim(VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a),
                             VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b), true);
    PG_RETURN_BOOL(s >= pg_tre_similarity_threshold);
}

/* `text <<<-> text` -> float4: 1 - strict_word_similarity(a,b). */
PG_FUNCTION_INFO_V1(pg_tre_strict_word_dist_op);
Datum
pg_tre_strict_word_dist_op(PG_FUNCTION_ARGS)
{
    text *a = PG_GETARG_TEXT_PP(0);
    text *b = PG_GETARG_TEXT_PP(1);
    float4 s = calc_word_sim(VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a),
                             VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b), true);
    PG_RETURN_FLOAT4(1.0f - s);
}
