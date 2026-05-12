/*
 * include/pg_tre/utf8.h - UTF-8 codepoint streaming for trigram extraction.
 *
 * Phase 3.5: migrate from byte-based trigrams to codepoint-based trigrams.
 * Pure ASCII is unchanged (each byte is a codepoint). Multi-byte UTF-8
 * characters are decoded to int32 codepoints before hashing.
 *
 * BREAKING CHANGE: indexes built on byte trigrams must be REINDEXed after
 * upgrade. PG_TRE_FORMAT_VERSION is bumped to detect this.
 */

#ifndef PG_TRE_UTF8_H
#define PG_TRE_UTF8_H

#include "postgres.h"

/*
 * Streaming UTF-8 decoder for trigram extraction.
 * Reads codepoints sequentially from a UTF-8 byte string.
 */
typedef struct PgTreCpStream
{
    const unsigned char *src;
    int src_len;
    int src_pos;
} PgTreCpStream;

/*
 * Initialize a codepoint stream over the given UTF-8 text.
 */
extern void pg_tre_cpstream_init(PgTreCpStream *s, const char *text, int len);

/*
 * Read the next codepoint from the stream.
 * Returns:
 *   0x0000..0x10FFFF: valid codepoint
 *   -1: end of stream
 *   -2: invalid UTF-8 sequence (ereport ERROR with context)
 */
extern int32 pg_tre_cpstream_next(PgTreCpStream *s);

/*
 * Return the current byte offset in the source string.
 * Used to record trigram positions (positions are byte offsets, not codepoint
 * indices, because TRE's recheck operates on byte strings).
 */
static inline int
pg_tre_cpstream_pos(const PgTreCpStream *s)
{
    return s->src_pos;
}

#endif /* PG_TRE_UTF8_H */
