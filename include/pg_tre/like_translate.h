/*
 * include/pg_tre/like_translate.h - LIKE/literal -> regex lowering
 * for Phase A / A1 operator-class acceleration.
 */
#ifndef PG_TRE_LIKE_TRANSLATE_H
#define PG_TRE_LIKE_TRANSLATE_H

/*
 * Translate a SQL LIKE pattern (length `len`, escape char `escape`,
 * or '\0' for no escape processing) into a palloc'd regex string the
 * pg_tre parser accepts.  Unanchored so the trigram extractor can
 * harvest interior literal runs.
 */
extern char *pg_tre_like_to_regex(const char *like, int len, char escape);

/*
 * Translate a plain literal (for the `=` strategy) into a palloc'd
 * anchored regex matching exactly that string.
 */
extern char *pg_tre_literal_to_regex(const char *lit, int len);

#endif /* PG_TRE_LIKE_TRANSLATE_H */
