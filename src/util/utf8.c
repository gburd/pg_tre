/*
 * src/util/utf8.c - character streaming for trigram extraction.
 *
 * Characters are decoded in the DATABASE ENCODING with PostgreSQL's own
 * per-encoding tables (pg_encoding_mblen_or_incomplete,
 * pg_encoding_verifymbchar, pg_encoding_mb2wchar_with_len) -- the same
 * functions core and pg_trgm use.  For UTF-8 the result is the Unicode
 * code point, exactly what this file decoded by hand before 4.3.0, so
 * every trigram hash in an existing UTF-8 index is unchanged.  For other
 * encodings it is PostgreSQL's pg_wchar for that encoding (LATIN1: the
 * byte value; EUC_*: the packed multibyte value), which is what the
 * patched TRE also sees, so index and matcher agree.
 *
 * (The file keeps its historical name; it is no longer UTF-8 only.)
 *
 * This decoder is strict: an invalid sequence ereports.  The server has
 * already verified the text on input, so in practice this only fires on
 * corrupt data.
 */

#include "postgres.h"

#include <limits.h>

#include "mb/pg_wchar.h"
#include "utils/elog.h"

#include "pg_tre/utf8.h"

void
pg_tre_cpstream_init(PgTreCpStream *s, const char *text, int len)
{
    s->src = (const unsigned char *) text;
    s->src_len = len;
    s->src_pos = 0;
}

/*
 * Decode one character of the database encoding from at most n bytes.
 * Returns its byte length and stores the character in *out; -1 for an
 * invalid sequence, -2 for one truncated by n.  Never ereports, so TRE can
 * call it too (see pg_tre_mbdecode in module.c).
 */
int
pg_tre_decode_char(const char *s, int n, pg_wchar *out)
{
    int         enc = GetDatabaseEncoding();
    int         len;
    pg_wchar    wbuf[MAX_MULTIBYTE_CHAR_LEN + 1];

    if (n <= 0)
        return -2;
    if (!IS_HIGHBIT_SET(*s))
    {
        /* ASCII is ASCII in every server encoding (all are ASCII supersets). */
        *out = (pg_wchar) (unsigned char) *s;
        return 1;
    }

    len = pg_encoding_mblen_or_incomplete(enc, s, (size_t) n);
    if (len == INT_MAX || len > n)
        return -2;
    if (pg_encoding_verifymbchar(enc, s, len) != len)
        return -1;
    /* The converter also writes a terminator: room for it (cf. 4.2.0). */
    (void) pg_encoding_mb2wchar_with_len(enc, s, wbuf, len);

    /*
     * Keep every character inside int32's non-negative range: callers carry
     * characters as int32 (the trigram tokenizer, the AST, TRE's wchar_t)
     * and reserve negatives for end-of-stream / error.  Every server
     * encoding's pg_wchar fits in 31 bits except EUC_TW's 4-byte CNS planes,
     * which pg_euctw2wchar packs as (0x8E << 24) | ...  -- above INT32_MAX.
     * Their low 24 bits are unique among EUC_TW characters (the plane byte
     * is 0xA1..0xB0 and no 1-3 byte EUC_TW character uses bits 24-30), so
     * fold the SS2 marker down to bit 30.  Without this such a character
     * read as -1 and ended the tokenizer early (index false negatives), or
     * stalled it in an endless loop (unbounded memory).
     */
    if (wbuf[0] > (pg_wchar) INT32_MAX)
        wbuf[0] = (wbuf[0] & 0x00FFFFFF) | 0x40000000;
    *out = wbuf[0];
    return len;
}

/*
 * Next character from the stream: >= 0 a character, -1 end of stream.
 * An invalid sequence ereports.
 */
int32
pg_tre_cpstream_next(PgTreCpStream *s)
{
    pg_wchar    wc;
    int         len;

    if (s->src_pos >= s->src_len)
        return -1;

    len = pg_tre_decode_char((const char *) s->src + s->src_pos,
                             s->src_len - s->src_pos, &wc);
    if (len < 0)
        ereport(ERROR,
                (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
                 errmsg("invalid byte sequence for encoding \"%s\" at byte offset %d",
                        GetDatabaseEncodingName(), s->src_pos)));
    s->src_pos += len;
    return (int32) wc;
}
