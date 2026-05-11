/*
 * src/util/type_pattern.c - tre_pattern composite type.
 *
 * tre_pattern encapsulates a regex pattern + edit budget + cost parameters.
 * Text format: "pattern@k" or "pattern@k:ins,del,subst" or just "pattern"
 *
 * Internal varlena layout:
 *   [varlena header]
 *   [int32 max_cost]
 *   [int32 cost_ins]
 *   [int32 cost_del]
 *   [int32 cost_subst]
 *   [int32 flags]
 *   [int32 pattern_len]
 *   [char pattern_bytes[pattern_len]]
 */

#include "postgres.h"

#include "fmgr.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "varatt.h"

#include "pg_tre/pg_tre.h"

#include <string.h>
#include <ctype.h>

typedef struct TrePatternData
{
	int32	vl_len_;		/* varlena header */
	int32	max_cost;
	int32	cost_ins;
	int32	cost_del;
	int32	cost_subst;
	int32	flags;
	int32	pattern_len;
	/* pattern bytes follow */
} TrePatternData;

typedef TrePatternData *TrePattern;

#define DatumGetTrePattern(X)	((TrePattern) PG_DETOAST_DATUM(X))
#define PG_GETARG_TRE_PATTERN(n) DatumGetTrePattern(PG_GETARG_DATUM(n))
#define PG_RETURN_TRE_PATTERN(x) PG_RETURN_POINTER(x)

/* Access macros */
#define TRE_PATTERN_DATA(p) ((char *)(p) + offsetof(TrePatternData, pattern_len) + sizeof(int32))
#define TRE_PATTERN_LEN(p)  ((p)->pattern_len)

/*
 * Parse a tre_pattern from text.
 * Formats:
 *   "pattern"
 *   "pattern@k"
 *   "pattern@k:ins,del,subst"
 */
static TrePattern
parse_tre_pattern(const char *str, int len)
{
	TrePattern result;
	const char *at_pos = NULL;
	const char *colon_pos = NULL;
	int pattern_len;
	int max_cost = 0;
	int cost_ins = 1, cost_del = 1, cost_subst = 1;
	Size total_size;
	int i;

	/* Find @ and : separators */
	for (i = 0; i < len; i++)
	{
		if (str[i] == '@' && at_pos == NULL)
			at_pos = str + i;
		else if (str[i] == ':' && at_pos != NULL && colon_pos == NULL)
			colon_pos = str + i;
	}

	if (at_pos != NULL)
	{
		/* Parse "@k" or "@k:ins,del,subst" */
		const char *num_start = at_pos + 1;
		const char *num_end = colon_pos ? colon_pos : (str + len);
		char *endptr;
		char buf[32];
		int num_len = num_end - num_start;

		if (num_len <= 0 || num_len >= sizeof(buf))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid tre_pattern format")));

		memcpy(buf, num_start, num_len);
		buf[num_len] = '\0';
		max_cost = (int) strtol(buf, &endptr, 10);
		if (*endptr != '\0' || max_cost < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid edit distance in tre_pattern")));

		if (colon_pos != NULL)
		{
			/* Parse ":ins,del,subst" */
			const char *cost_start = colon_pos + 1;
			int n;
			
			n = sscanf(cost_start, "%d,%d,%d", &cost_ins, &cost_del, &cost_subst);
			if (n != 3)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid cost specification in tre_pattern")));
		}

		pattern_len = at_pos - str;
	}
	else
	{
		/* Just "pattern", no @ */
		pattern_len = len;
	}

	/* Allocate result */
	total_size = offsetof(TrePatternData, pattern_len) + sizeof(int32) + pattern_len;
	result = (TrePattern) palloc0(total_size);
	SET_VARSIZE(result, total_size);
	result->max_cost = max_cost;
	result->cost_ins = cost_ins;
	result->cost_del = cost_del;
	result->cost_subst = cost_subst;
	result->flags = 0;
	result->pattern_len = pattern_len;
	memcpy(TRE_PATTERN_DATA(result), str, pattern_len);

	return result;
}

/*
 * Format a tre_pattern as text.
 */
static char *
format_tre_pattern(TrePattern pat)
{
	StringInfoData buf;
	const char *pattern = TRE_PATTERN_DATA(pat);
	int pattern_len = pat->pattern_len;

	initStringInfo(&buf);
	appendBinaryStringInfo(&buf, pattern, pattern_len);

	if (pat->max_cost > 0)
	{
		appendStringInfo(&buf, "@%d", pat->max_cost);
		if (pat->cost_ins != 1 || pat->cost_del != 1 || pat->cost_subst != 1)
		{
			appendStringInfo(&buf, ":%d,%d,%d",
							 pat->cost_ins, pat->cost_del, pat->cost_subst);
		}
	}

	return buf.data;
}

/* ---- SQL functions ---- */

PG_FUNCTION_INFO_V1(tre_pattern_in);
Datum
tre_pattern_in(PG_FUNCTION_ARGS)
{
	char *str = PG_GETARG_CSTRING(0);
	TrePattern result = parse_tre_pattern(str, strlen(str));
	PG_RETURN_TRE_PATTERN(result);
}

PG_FUNCTION_INFO_V1(tre_pattern_out);
Datum
tre_pattern_out(PG_FUNCTION_ARGS)
{
	TrePattern pat = PG_GETARG_TRE_PATTERN(0);
	PG_RETURN_CSTRING(format_tre_pattern(pat));
}

PG_FUNCTION_INFO_V1(tre_pattern_recv);
Datum
tre_pattern_recv(PG_FUNCTION_ARGS)
{
	StringInfo buf = (StringInfo) PG_GETARG_POINTER(0);
	TrePattern result;
	int32 max_cost, cost_ins, cost_del, cost_subst, flags, pattern_len;
	Size total_size;

	max_cost = pq_getmsgint(buf, sizeof(int32));
	cost_ins = pq_getmsgint(buf, sizeof(int32));
	cost_del = pq_getmsgint(buf, sizeof(int32));
	cost_subst = pq_getmsgint(buf, sizeof(int32));
	flags = pq_getmsgint(buf, sizeof(int32));
	pattern_len = pq_getmsgint(buf, sizeof(int32));

	if (pattern_len < 0 || pattern_len > 10485760) /* 10MB sanity check */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid pattern length in tre_pattern")));

	total_size = offsetof(TrePatternData, pattern_len) + sizeof(int32) + pattern_len;
	result = (TrePattern) palloc0(total_size);
	SET_VARSIZE(result, total_size);
	result->max_cost = max_cost;
	result->cost_ins = cost_ins;
	result->cost_del = cost_del;
	result->cost_subst = cost_subst;
	result->flags = flags;
	result->pattern_len = pattern_len;

	pq_copymsgbytes(buf, TRE_PATTERN_DATA(result), pattern_len);

	PG_RETURN_TRE_PATTERN(result);
}

PG_FUNCTION_INFO_V1(tre_pattern_send);
Datum
tre_pattern_send(PG_FUNCTION_ARGS)
{
	TrePattern pat = PG_GETARG_TRE_PATTERN(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, pat->max_cost);
	pq_sendint32(&buf, pat->cost_ins);
	pq_sendint32(&buf, pat->cost_del);
	pq_sendint32(&buf, pat->cost_subst);
	pq_sendint32(&buf, pat->flags);
	pq_sendint32(&buf, pat->pattern_len);
	pq_sendbytes(&buf, TRE_PATTERN_DATA(pat), pat->pattern_len);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/* ---- Constructor functions ---- */

PG_FUNCTION_INFO_V1(tre_pattern_make);
Datum
tre_pattern_make(PG_FUNCTION_ARGS)
{
	text *pattern_text = PG_GETARG_TEXT_PP(0);
	char *pattern = VARDATA_ANY(pattern_text);
	int pattern_len = VARSIZE_ANY_EXHDR(pattern_text);
	TrePattern result;
	Size total_size;

	total_size = offsetof(TrePatternData, pattern_len) + sizeof(int32) + pattern_len;
	result = (TrePattern) palloc0(total_size);
	SET_VARSIZE(result, total_size);
	result->max_cost = 0;
	result->cost_ins = 1;
	result->cost_del = 1;
	result->cost_subst = 1;
	result->flags = 0;
	result->pattern_len = pattern_len;
	memcpy(TRE_PATTERN_DATA(result), pattern, pattern_len);

	PG_RETURN_TRE_PATTERN(result);
}

PG_FUNCTION_INFO_V1(tre_pattern_make_k);
Datum
tre_pattern_make_k(PG_FUNCTION_ARGS)
{
	text *pattern_text = PG_GETARG_TEXT_PP(0);
	int32 max_cost = PG_GETARG_INT32(1);
	char *pattern = VARDATA_ANY(pattern_text);
	int pattern_len = VARSIZE_ANY_EXHDR(pattern_text);
	TrePattern result;
	Size total_size;

	if (max_cost < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("max_cost must be non-negative")));

	total_size = offsetof(TrePatternData, pattern_len) + sizeof(int32) + pattern_len;
	result = (TrePattern) palloc0(total_size);
	SET_VARSIZE(result, total_size);
	result->max_cost = max_cost;
	result->cost_ins = 1;
	result->cost_del = 1;
	result->cost_subst = 1;
	result->flags = 0;
	result->pattern_len = pattern_len;
	memcpy(TRE_PATTERN_DATA(result), pattern, pattern_len);

	PG_RETURN_TRE_PATTERN(result);
}

PG_FUNCTION_INFO_V1(tre_pattern_make_full);
Datum
tre_pattern_make_full(PG_FUNCTION_ARGS)
{
	text *pattern_text = PG_GETARG_TEXT_PP(0);
	int32 max_cost = PG_GETARG_INT32(1);
	int32 cost_ins = PG_GETARG_INT32(2);
	int32 cost_del = PG_GETARG_INT32(3);
	int32 cost_subst = PG_GETARG_INT32(4);
	char *pattern = VARDATA_ANY(pattern_text);
	int pattern_len = VARSIZE_ANY_EXHDR(pattern_text);
	TrePattern result;
	Size total_size;

	if (max_cost < 0 || cost_ins < 0 || cost_del < 0 || cost_subst < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cost values must be non-negative")));

	total_size = offsetof(TrePatternData, pattern_len) + sizeof(int32) + pattern_len;
	result = (TrePattern) palloc0(total_size);
	SET_VARSIZE(result, total_size);
	result->max_cost = max_cost;
	result->cost_ins = cost_ins;
	result->cost_del = cost_del;
	result->cost_subst = cost_subst;
	result->flags = 0;
	result->pattern_len = pattern_len;
	memcpy(TRE_PATTERN_DATA(result), pattern, pattern_len);

	PG_RETURN_TRE_PATTERN(result);
}

/* ---- Match operator implementation ---- */

PG_FUNCTION_INFO_V1(tre_match_scalar);
Datum
tre_match_scalar(PG_FUNCTION_ARGS)
{
	text *haystack_text = PG_GETARG_TEXT_PP(0);
	TrePattern pat = PG_GETARG_TRE_PATTERN(1);
	char *pattern = TRE_PATTERN_DATA(pat);
	int pattern_len = pat->pattern_len;
	text *pat_txt;
	bool result;

	/* Build a text value from the pattern */
	pat_txt = cstring_to_text_with_len(pattern, pattern_len);
	
	/* Call the legacy tre_amatch function */
	result = DatumGetBool(DirectFunctionCall3(pg_tre_amatch,
											  PointerGetDatum(haystack_text),
											  PointerGetDatum(pat_txt),
											  Int32GetDatum(pat->max_cost)));

	PG_RETURN_BOOL(result);
}

/* ---- Accessors consumed by src/am/amscan.c ---- */

char *
tre_pattern_get_text(TrePattern p, int *len_out)
{
	if (len_out)
		*len_out = p->pattern_len;
	return TRE_PATTERN_DATA(p);
}

int32
tre_pattern_get_max_cost(TrePattern p)
{
	return p->max_cost;
}
