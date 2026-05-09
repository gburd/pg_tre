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
extern void *tre_parseAlloc(void *(*mallocProc)(size_t), struct TreParseCtx *ctx);
extern void tre_parseFree(void *parser, void (*freeProc)(void *));
extern void tre_parse(void *parser, int token_kind, TreToken token_value,
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
	void *parser;
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

	/* Allocate parser */
	parser = tre_parseAlloc(malloc, ctx);
	if (parser == NULL)
		elog(ERROR, "failed to allocate regex parser");

	/* Feed tokens to the parser */
	while ((tok_kind = tre_tokenize_next(ctx, &tok)) > 0)
	{
		if (ctx->syntax_error)
			break;

		tre_parse(parser, tok_kind, tok, ctx);

		if (ctx->syntax_error)
			break;
	}

	/* Check for tokenizer error */
	if (tok_kind < 0)
	{
		/* Error already set by tokenizer */
		tre_parseFree(parser, free);
		return false;
	}

	/* Send EOF token (token 0) */
	if (!ctx->syntax_error)
	{
		memset(&tok, 0, sizeof(tok));
		tre_parse(parser, 0, tok, ctx);
	}

	/* Clean up */
	tre_parseFree(parser, free);

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
