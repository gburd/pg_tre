/*
 * src/query/like_translate.c - translate SQL LIKE patterns and plain
 * literals into the regex syntax pg_tre's parser accepts, for the
 * LIKE/ILIKE/=/~ operator-class acceleration (Phase A / A1).
 *
 * NORTH STAR: pg_tre is a REGEX index with edit distance.  A1 does
 * not add a new matcher: it lowers LIKE/=/regex into the SAME
 * trigram-extraction path %~~ already uses, at k=0.  The executor
 * rechecks every candidate with the real built-in operator
 * (textlike / texticlike / texteq / textregexeq), so the index is
 * only ever a (lossy) candidate filter -- correctness is the
 * executor's, exactly as with pg_trgm GIN.
 *
 * We escape every regex metacharacter that is NOT a LIKE
 * metacharacter so the translated string is a faithful regex.
 */

#include "postgres.h"

#include "fmgr.h"
#include "varatt.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"

#include "pg_tre/like_translate.h"

/* Append c to buf, regex-escaping it if it is a regex metacharacter. */
static void
append_escaped(StringInfo buf, char c)
{
    switch (c)
    {
        case '.': case '*': case '+': case '?': case '(': case ')':
        case '[': case ']': case '{': case '}': case '^': case '$':
        case '|': case '\\':
            appendStringInfoChar(buf, '\\');
            /* fall through */
        default:
            appendStringInfoChar(buf, c);
    }
}

/*
 * Translate a SQL LIKE pattern into a regex string.
 *
 *   %  -> .*       (any run, including empty)
 *   _  -> .        (any single character)
 *   \x -> literal x  (LIKE escape; default escape char is backslash)
 *   anything else -> literal (regex-escaped)
 *
 * The result is NOT anchored: LIKE matches the whole string, but for
 * *trigram extraction* we only need the literal runs between
 * wildcards, and the executor recheck enforces full LIKE semantics.
 * Leaving it unanchored lets the extractor harvest interior literal
 * runs (e.g. LIKE '%foo%' -> '.*foo.*' -> trigrams of "foo").
 */
char *
pg_tre_like_to_regex(const char *like, int len, char escape)
{
    StringInfoData buf;
    int i;

    initStringInfo(&buf);
    for (i = 0; i < len; i++)
    {
        char c = like[i];

        if (escape != '\0' && c == escape)
        {
            /* Next char is a literal, even if it is % or _. */
            i++;
            if (i < len)
                append_escaped(&buf, like[i]);
            else
                append_escaped(&buf, escape);   /* trailing escape: literal */
            continue;
        }
        if (c == '%')
            appendStringInfoString(&buf, ".*");
        else if (c == '_')
            appendStringInfoChar(&buf, '.');
        else
            append_escaped(&buf, c);
    }
    return buf.data;   /* palloc'd in current context */
}

/*
 * Translate a plain literal string (for the `=` strategy) into an
 * anchored regex that matches exactly that string.  Every character
 * is regex-escaped.  Anchoring is advisory for the extractor; the
 * executor recheck (texteq) enforces exact equality.
 */
char *
pg_tre_literal_to_regex(const char *lit, int len)
{
    StringInfoData buf;
    int i;

    initStringInfo(&buf);
    appendStringInfoChar(&buf, '^');
    for (i = 0; i < len; i++)
        append_escaped(&buf, lit[i]);
    appendStringInfoChar(&buf, '$');
    return buf.data;
}
