/*
 * src/util/utf8.c - UTF-8 codepoint streaming for trigram extraction.
 *
 * Phase 3.5: migrate from byte-based trigrams to codepoint-based trigrams.
 *
 * This decoder is strict: invalid UTF-8 triggers ereport(ERROR). This is
 * appropriate for indexed values (we don't want garbage in the index) and
 * query patterns (the user must fix the query). If we encounter invalid
 * UTF-8 in production, it indicates either:
 *   1. The text column contains non-UTF-8 data (fix: validate at insert time)
 *   2. The database encoding is not UTF-8 (pg_tre requires UTF-8)
 *
 * We use PostgreSQL's built-in UTF-8 validation to ensure correctness.
 */

#include "postgres.h"

#include "mb/pg_wchar.h"
#include "utils/elog.h"

#include "pg_tre/utf8.h"

/*
 * Initialize a codepoint stream over UTF-8 text.
 */
void
pg_tre_cpstream_init(PgTreCpStream *s, const char *text, int len)
{
    s->src = (const unsigned char *) text;
    s->src_len = len;
    s->src_pos = 0;
}

/*
 * Read the next codepoint from the stream.
 * Returns:
 *   0x0000..0x10FFFF: valid Unicode codepoint
 *   -1: end of stream
 *   -2: invalid UTF-8 (after ereport ERROR, so this never actually returns)
 *
 * Strategy: use PostgreSQL's pg_utf_mblen to get the byte length of the
 * UTF-8 sequence, then decode manually. We validate the sequence before
 * decoding to ensure we never see partial or invalid sequences.
 */
int32
pg_tre_cpstream_next(PgTreCpStream *s)
{
    int mblen;
    int32 codepoint;
    unsigned char c0, c1, c2, c3;

    if (s->src_pos >= s->src_len)
        return -1;  /* end of stream */

    c0 = s->src[s->src_pos];

    /*
     * Fast path for ASCII (most common case).
     */
    if (c0 < 0x80)
    {
        s->src_pos++;
        return (int32) c0;
    }

    /*
     * Multi-byte UTF-8 sequence. Use pg_utf_mblen to determine the length.
     */
    mblen = pg_utf_mblen((const unsigned char *) &s->src[s->src_pos]);

    /*
     * Validate that we have enough bytes and the sequence is legal.
     */
    if (s->src_pos + mblen > s->src_len)
    {
        ereport(ERROR,
                (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
                 errmsg("invalid UTF-8 sequence at byte offset %d: incomplete sequence at end of string",
                        s->src_pos)));
        return -2;  /* unreachable */
    }

    /*
     * Decode the UTF-8 sequence manually. We validate as we go.
     */
    switch (mblen)
    {
        case 2:
            /* 110xxxxx 10xxxxxx */
            c1 = s->src[s->src_pos + 1];
            if ((c0 & 0xE0) != 0xC0 || (c1 & 0xC0) != 0x80)
                goto invalid_sequence;
            codepoint = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
            /* Reject overlong encodings (must be >= 0x80) */
            if (codepoint < 0x80)
                goto invalid_sequence;
            break;

        case 3:
            /* 1110xxxx 10xxxxxx 10xxxxxx */
            c1 = s->src[s->src_pos + 1];
            c2 = s->src[s->src_pos + 2];
            if ((c0 & 0xF0) != 0xE0 || (c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80)
                goto invalid_sequence;
            codepoint = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            /* Reject overlong encodings (must be >= 0x800) and surrogates (0xD800..0xDFFF) */
            if (codepoint < 0x800 || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
                goto invalid_sequence;
            break;

        case 4:
            /* 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
            c1 = s->src[s->src_pos + 1];
            c2 = s->src[s->src_pos + 2];
            c3 = s->src[s->src_pos + 3];
            if ((c0 & 0xF8) != 0xF0 || (c1 & 0xC0) != 0x80 ||
                (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
                goto invalid_sequence;
            codepoint = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) |
                        ((c2 & 0x3F) << 6) | (c3 & 0x3F);
            /* Reject overlong encodings (must be >= 0x10000) and out-of-range (> 0x10FFFF) */
            if (codepoint < 0x10000 || codepoint > 0x10FFFF)
                goto invalid_sequence;
            break;

        default:
            /* Invalid UTF-8 lead byte (5- or 6-byte sequences are not valid UTF-8) */
            goto invalid_sequence;
    }

    s->src_pos += mblen;
    return codepoint;

invalid_sequence:
    ereport(ERROR,
            (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
             errmsg("invalid UTF-8 sequence at byte offset %d: lead byte 0x%02X",
                    s->src_pos, c0)));
    return -2;  /* unreachable */
}
