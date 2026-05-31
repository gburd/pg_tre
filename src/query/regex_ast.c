/*
 * src/query/regex_ast.c - regex AST allocator + constructors.
 *
 * All AST nodes allocated in ctx->mcxt via palloc.
 * Phase 3: ASCII-only support for literal codepoints (single bytes).
 * Phase 3.5: full Unicode support with proper UTF-8 decoding.
 */

#include "postgres.h"

#include "miscadmin.h"
#include "pg_tre/regex_ast.h"
#include "utils/memutils.h"

#include <string.h>

/*
 * Allocate a new AST node in the context.
 */
static RegexAst *
alloc_node(TreParseCtx *ctx, RegexAstKind kind)
{
	RegexAst *node;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(ctx->mcxt);
	node = (RegexAst *) palloc0(sizeof(RegexAst));
	node->kind = kind;
	node->span_start = -1;
	node->span_end = -1;
	MemoryContextSwitchTo(oldcxt);

	return node;
}

/*
 * Allocate a RegexClass structure.
 */
static RegexClass *
alloc_class(TreParseCtx *ctx, int32 n_ranges)
{
	RegexClass *cls;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(ctx->mcxt);
	cls = (RegexClass *) palloc0(sizeof(RegexClass));
	cls->n_ranges = n_ranges;
	if (n_ranges > 0)
	{
		cls->lo = (int32 *) palloc0(n_ranges * sizeof(int32));
		cls->hi = (int32 *) palloc0(n_ranges * sizeof(int32));
	}
	MemoryContextSwitchTo(oldcxt);

	return cls;
}

/*
 * Merge and normalize ranges in a character class.
 * Sorts ranges and merges overlapping ones.
 */
static void
normalize_class(RegexClass *cls)
{
	int i, j, n;

	if (cls->n_ranges <= 1)
		return;

	/* Bubble sort (classes are typically small) */
	for (i = 0; i < cls->n_ranges - 1; i++)
	{
		for (j = i + 1; j < cls->n_ranges; j++)
		{
			if (cls->lo[i] > cls->lo[j])
			{
				int32 tmp;
				tmp = cls->lo[i];
				cls->lo[i] = cls->lo[j];
				cls->lo[j] = tmp;
				tmp = cls->hi[i];
				cls->hi[i] = cls->hi[j];
				cls->hi[j] = tmp;
			}
		}
	}

	/* Merge overlapping ranges */
	n = 0;
	for (i = 0; i < cls->n_ranges; i++)
	{
		if (n == 0)
		{
			cls->lo[n] = cls->lo[i];
			cls->hi[n] = cls->hi[i];
			n++;
		}
		else
		{
			/* Check if [lo[i], hi[i]] overlaps or is adjacent to [lo[n-1], hi[n-1]] */
			if (cls->lo[i] <= cls->hi[n-1] + 1)
			{
				/* Merge */
				if (cls->hi[i] > cls->hi[n-1])
					cls->hi[n-1] = cls->hi[i];
			}
			else
			{
				/* Non-overlapping, start new range */
				cls->lo[n] = cls->lo[i];
				cls->hi[n] = cls->hi[i];
				n++;
			}
		}
	}
	cls->n_ranges = n;
}

/* ---- AST constructors ---- */

RegexAst *
regex_ast_literal(TreParseCtx *ctx, int32 cp)
{
	RegexAst *node = alloc_node(ctx, REGEX_AST_LITERAL);
	node->u.literal.codepoint = cp;
	return node;
}

RegexAst *
regex_ast_any(TreParseCtx *ctx)
{
	return alloc_node(ctx, REGEX_AST_ANY);
}

RegexAst *
regex_ast_anchor(TreParseCtx *ctx, RegexAnchor which)
{
	RegexAst *node = alloc_node(ctx, REGEX_AST_ANCHOR);
	node->u.anchor.which = which;
	return node;
}

RegexAst *
regex_ast_concat(TreParseCtx *ctx, RegexAst *l, RegexAst *r)
{
	RegexAst *node = alloc_node(ctx, REGEX_AST_CONCAT);
	node->u.concat.left = l;
	node->u.concat.right = r;
	return node;
}

RegexAst *
regex_ast_alt(TreParseCtx *ctx, RegexAst *l, RegexAst *r)
{
	RegexAst *node = alloc_node(ctx, REGEX_AST_ALT);
	node->u.alt.left = l;
	node->u.alt.right = r;
	return node;
}

RegexAst *
regex_ast_rep(TreParseCtx *ctx, RegexAst *child, int32 m, int32 n)
{
	RegexAst *node = alloc_node(ctx, REGEX_AST_REP);
	node->u.rep.child = child;
	node->u.rep.min_rep = m;
	node->u.rep.max_rep = n;
	return node;
}

RegexAst *
regex_ast_approx(TreParseCtx *ctx, RegexAst *child, int32 k)
{
	RegexAst *node = alloc_node(ctx, REGEX_AST_APPROX);
	node->u.approx.child = child;
	node->u.approx.k = k;
	return node;
}

RegexAst *
regex_ast_class(TreParseCtx *ctx, RegexClass *body, bool negated)
{
	RegexAst *node = alloc_node(ctx, REGEX_AST_CLASS);
	normalize_class(body);
	node->u.charclass.cs = body;
	node->u.charclass.negated = negated;
	return node;
}

RegexClass *
regex_ast_class_char(TreParseCtx *ctx, int32 cp)
{
	RegexClass *cls = alloc_class(ctx, 1);
	cls->lo[0] = cp;
	cls->hi[0] = cp;
	return cls;
}

RegexClass *
regex_ast_class_range(TreParseCtx *ctx, int32 lo, int32 hi)
{
	RegexClass *cls = alloc_class(ctx, 1);
	if (lo > hi)
	{
		ctx->syntax_error = true;
		snprintf(ctx->errmsg, sizeof(ctx->errmsg),
				 "invalid character range [%d-%d]", lo, hi);
		return cls;
	}
	cls->lo[0] = lo;
	cls->hi[0] = hi;
	return cls;
}

RegexClass *
regex_ast_class_union(TreParseCtx *ctx, RegexClass *a, RegexClass *b)
{
	RegexClass *cls;
	MemoryContext oldcxt;
	int i;

	oldcxt = MemoryContextSwitchTo(ctx->mcxt);
	cls = alloc_class(ctx, a->n_ranges + b->n_ranges);

	/* Copy ranges from a */
	for (i = 0; i < a->n_ranges; i++)
	{
		cls->lo[i] = a->lo[i];
		cls->hi[i] = a->hi[i];
	}

	/* Append ranges from b */
	for (i = 0; i < b->n_ranges; i++)
	{
		cls->lo[a->n_ranges + i] = b->lo[i];
		cls->hi[a->n_ranges + i] = b->hi[i];
	}

	normalize_class(cls);
	MemoryContextSwitchTo(oldcxt);

	return cls;
}

/* ---- Debug pretty-printer ---- */

static void
dump_ast_internal(StringInfo buf, RegexAst *node, int depth)
{
	int i;

	check_stack_depth();

	if (node == NULL)
	{
		appendStringInfoString(buf, "(null)");
		return;
	}

	for (i = 0; i < depth; i++)
		appendStringInfoString(buf, "  ");

	switch (node->kind)
	{
		case REGEX_AST_LITERAL:
			appendStringInfo(buf, "LITERAL(%c)", (char) node->u.literal.codepoint);
			break;

		case REGEX_AST_ANY:
			appendStringInfoString(buf, "ANY(.)");
			break;

		case REGEX_AST_CLASS:
			{
				RegexClass *cs = node->u.charclass.cs;
				appendStringInfo(buf, "CLASS(%s", node->u.charclass.negated ? "^" : "");
				for (i = 0; i < cs->n_ranges; i++)
				{
					if (cs->lo[i] == cs->hi[i])
						appendStringInfo(buf, "%c", (char) cs->lo[i]);
					else
						appendStringInfo(buf, "%c-%c", (char) cs->lo[i], (char) cs->hi[i]);
				}
				appendStringInfoString(buf, ")");
			}
			break;

		case REGEX_AST_ANCHOR:
			appendStringInfo(buf, "ANCHOR(%s)",
							 node->u.anchor.which == REGEX_ANCHOR_START ? "^" : "$");
			break;

		case REGEX_AST_CONCAT:
			appendStringInfoString(buf, "CONCAT\n");
			dump_ast_internal(buf, node->u.concat.left, depth + 1);
			appendStringInfoChar(buf, '\n');
			dump_ast_internal(buf, node->u.concat.right, depth + 1);
			break;

		case REGEX_AST_ALT:
			appendStringInfoString(buf, "ALT\n");
			dump_ast_internal(buf, node->u.alt.left, depth + 1);
			appendStringInfoChar(buf, '\n');
			dump_ast_internal(buf, node->u.alt.right, depth + 1);
			break;

		case REGEX_AST_REP:
			appendStringInfo(buf, "REP{%d,%d}\n",
							 node->u.rep.min_rep,
							 node->u.rep.max_rep == -1 ? -1 : node->u.rep.max_rep);
			dump_ast_internal(buf, node->u.rep.child, depth + 1);
			break;

		case REGEX_AST_APPROX:
			appendStringInfo(buf, "APPROX{~%d}\n", node->u.approx.k);
			dump_ast_internal(buf, node->u.approx.child, depth + 1);
			break;

		default:
			appendStringInfo(buf, "(unknown kind %d)", node->kind);
			break;
	}
}

/*
 * Pretty-print the AST for debugging.
 */
char *
regex_ast_debug_dump(RegexAst *root)
{
	StringInfoData buf;
	initStringInfo(&buf);
	dump_ast_internal(&buf, root, 0);
	return buf.data;
}
