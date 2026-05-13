/*
 * src/query/debug.c - debug UDFs for testing regex parser and extraction.
 *
 * Phase 3: expose tre_parse_debug() and tre_extract_debug() for differential
 * testing.
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "varatt.h"

#include "pg_tre/regex_ast.h"

PG_FUNCTION_INFO_V1(tre_parse_debug);

/*
 * tre_parse_debug(text) -> text
 *
 * Parse a regex and return a pretty-printed AST dump.
 */
Datum
tre_parse_debug(PG_FUNCTION_ARGS)
{
	text *pattern_text = PG_GETARG_TEXT_PP(0);
	char *pattern = VARDATA_ANY(pattern_text);
	int pattern_len = VARSIZE_ANY_EXHDR(pattern_text);
	TreParseCtx ctx;
	char *result_str;

	/* Parse the regex */
	if (!tre_parse_regex(&ctx, pattern, pattern_len))
	{
		/* Syntax error */
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("regex syntax error: %s", ctx.errmsg)));
	}

	/* Dump the AST */
	result_str = regex_ast_debug_dump(ctx.root);

	PG_RETURN_TEXT_P(cstring_to_text(result_str));
}

PG_FUNCTION_INFO_V1(tre_extract_debug);
PG_FUNCTION_INFO_V1(tre_extract_debug_k);

/*
 * tre_extract_debug(text) -> text
 * tre_extract_debug(text, int) -> text
 *
 * Parse a regex, extract trigrams, and return a debug dump of the query
 * tree.  The optional second argument is the global edit budget (k).
 */
static Datum tre_extract_debug_impl(text *pattern_text, int32 max_cost);

Datum
tre_extract_debug(PG_FUNCTION_ARGS)
{
	return tre_extract_debug_impl(PG_GETARG_TEXT_PP(0), 0);
}

Datum
tre_extract_debug_k(PG_FUNCTION_ARGS)
{
	return tre_extract_debug_impl(PG_GETARG_TEXT_PP(0), PG_GETARG_INT32(1));
}

static Datum
tre_extract_debug_impl(text *pattern_text, int32 max_cost)
{
	char *pattern = VARDATA_ANY(pattern_text);
	int pattern_len = VARSIZE_ANY_EXHDR(pattern_text);
	TreParseCtx ctx;
	TrigramQuery query;
	StringInfoData buf;
	int i, j;

	/* Parse the regex */
	if (!tre_parse_regex(&ctx, pattern, pattern_len))
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("regex syntax error: %s", ctx.errmsg)));
	}

	/* Extract trigrams */
	if (!regex_extract_query(&ctx, max_cost, &query))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("trigram extraction failed")));
	}

	/* Format the query */
	initStringInfo(&buf);

	appendStringInfo(&buf, "k=%d mode=%s\n", max_cost,
					 query.mode == TRIGRAM_QUERY_DNF ? "DNF" : "CNF");

	if (query.always_true)
	{
		appendStringInfoString(&buf, "ALWAYS_TRUE (extraction defeated)");
	}
	else if (query.n == 0)
	{
		appendStringInfoString(&buf, "EMPTY (no trigrams)");
	}
	else
	{
		appendStringInfo(&buf, "%s of %d %s:\n",
						 query.mode == TRIGRAM_QUERY_DNF ? "OR" : "AND",
						 query.n,
						 query.mode == TRIGRAM_QUERY_DNF ? "tiles" : "conjuncts");
		for (i = 0; i < query.n; i++)
		{
			appendStringInfo(&buf, "  %s %d: %d alternatives:\n",
							 query.mode == TRIGRAM_QUERY_DNF ? "Tile" : "Conjunct",
							 i, query.conjuncts[i].n);
			for (j = 0; j < query.conjuncts[i].n; j++)
			{
				appendStringInfo(&buf, "    trigram_hash=%lu [%d,%d]\n",
								 (unsigned long) query.conjuncts[i].alts[j].trigram_hash,
								 query.conjuncts[i].alts[j].min_offset,
								 query.conjuncts[i].alts[j].max_offset);
			}
		}
	}

	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}
