/*
 * src/query/parser.c - regex parser driver.
 *
 * Wraps the Lime-generated parser and tokenizer into a single
 * tre_parse_regex() entry point.
 */

#include "postgres.h"

#include "pg_tre/regex_ast.h"
#include "utils/memutils.h"

#include <string.h>

/* Lime-generated parser interface from tre_grammar.c */
extern void *pg_tre_rx_parseAlloc(void *(*mallocProc)(size_t), struct TreParseCtx *ctx);
extern void pg_tre_rx_parseFree(void *parser, void (*freeProc)(void *));
extern void pg_tre_rx_parse(void *parser, int token_kind, TreToken token_value,
					  struct TreParseCtx *ctx);

/*
 * Parse a regex pattern into an AST.
 *
 * On success, ctx->root is set to the AST root and true is returned.
 * On failure, ctx->syntax_error is true and ctx->errmsg is filled.
 *
 * All AST nodes are allocated in ctx->mcxt.
 */
bool
tre_parse_regex(TreParseCtx *ctx, const char *pattern, int len)
{
	volatile void *parser_v = NULL;
	TreToken tok;
	int tok_kind;

	/* Initialize context */
	memset(ctx, 0, sizeof(TreParseCtx));
	ctx->input = pattern;
	ctx->input_len = len;
	ctx->mcxt = CurrentMemoryContext;
	ctx->root = NULL;
	ctx->syntax_error = false;
	ctx->tokenizer_state = NULL;

	/*
	 * The Lime-generated parser is allocated with malloc() and must be
	 * freed with the matching free().  Wrap the parse in PG_TRY/PG_CATCH
	 * so the parser is freed even when the tokenizer or AST builders raise
	 * via ereport(ERROR).  Without this, every malformed input that hits
	 * an ereport() leaks ~2 KB of parser state for the lifetime of the
	 * backend (discovered via libFuzzer; see fuzz/RUN_REPORT.md).
	 */
	PG_TRY();
	{
		parser_v = pg_tre_rx_parseAlloc(malloc, ctx);
		if (parser_v == NULL)
			elog(ERROR, "failed to allocate regex parser");

		while ((tok_kind = tre_tokenize_next(ctx, &tok)) > 0)
		{
			if (ctx->syntax_error)
				break;

			pg_tre_rx_parse((void *) parser_v, tok_kind, tok, ctx);

			if (ctx->syntax_error)
				break;
		}

		if (tok_kind < 0)
		{
			/* Tokenizer error already set the error message. */
		}
		else if (!ctx->syntax_error)
		{
			/* Send EOF token (token 0). */
			memset(&tok, 0, sizeof(tok));
			pg_tre_rx_parse((void *) parser_v, 0, tok, ctx);
		}
	}
	PG_FINALLY();
	{
		if (parser_v != NULL)
			pg_tre_rx_parseFree((void *) parser_v, free);
	}
	PG_END_TRY();

	/* Check for syntax error */
	if (ctx->syntax_error)
		return false;

	/* Check that we got a root */
	if (ctx->root == NULL)
	{
		ctx->syntax_error = true;
		snprintf(ctx->errmsg, sizeof(ctx->errmsg), "empty pattern");
		return false;
	}

	return true;
}
