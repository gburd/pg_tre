/*
 * include/pg_tre/regex_ast.h - regex AST shared between parser,
 * extraction, and recheck.
 *
 * CONTRACT FILE.  Agent B (Phase 3) is primary owner.  Later phases
 * (5 for approximate extraction, 6 for cost estimation) extend this
 * header in place -- additions are backwards compatible; do not
 * reorder or remove existing enum values without bumping callers.
 */

#ifndef PG_TRE_REGEX_AST_H
#define PG_TRE_REGEX_AST_H

#include "postgres.h"
#include "nodes/memnodes.h"

typedef enum RegexAstKind
{
    REGEX_AST_LITERAL   = 1,   /* single codepoint */
    REGEX_AST_ANY       = 2,   /* . */
    REGEX_AST_CLASS     = 3,   /* [abc] or [^abc] */
    REGEX_AST_ANCHOR    = 4,   /* ^ or $ */
    REGEX_AST_CONCAT    = 5,
    REGEX_AST_ALT       = 6,
    REGEX_AST_REP       = 7,   /* {m,n} with m>=0, n=-1 for unbounded */
    REGEX_AST_APPROX    = 8    /* TRE {~m} local edit budget */
} RegexAstKind;

typedef enum RegexAnchor
{
    REGEX_ANCHOR_START = 1,
    REGEX_ANCHOR_END   = 2
} RegexAnchor;

typedef struct RegexAst
{
    RegexAstKind      kind;
    /* Union over kind-specific payload follows.  Keep this small so
     * pattern parsing stays allocation-cheap. */
    union
    {
        struct { int32 codepoint; }                     literal;
        struct { bool  negated; struct RegexClass *cs; } charclass;
        struct { RegexAnchor which; }                   anchor;
        struct { struct RegexAst *left, *right; }       concat;
        struct { struct RegexAst *left, *right; }       alt;
        struct { struct RegexAst *child;
                 int32 min_rep, max_rep; }              rep;     /* max_rep=-1 => unbounded */
        struct { struct RegexAst *child; int32 k; }     approx;
    } u;
    /* Source span for diagnostics (byte offsets into original pattern). */
    int32 span_start;
    int32 span_end;
} RegexAst;

typedef struct RegexClass
{
    /* A sorted, disjoint set of (lo, hi) inclusive codepoint ranges.
     * Phase 3 supports ASCII; Phase 3.5 extends to full Unicode. */
    int32  n_ranges;
    int32 *lo;
    int32 *hi;
} RegexClass;

typedef struct TreParseCtx
{
    MemoryContext   mcxt;
    const char     *input;
    int             input_len;
    RegexAst       *root;
    bool            syntax_error;
    char            errmsg[128];
    /* Used by the Lime-generated parser to shuttle tokens. */
    int             cur_pos;
} TreParseCtx;

typedef struct TreToken
{
    int32 kind;
    int32 cp;         /* codepoint for LITERAL */
    int32 i;          /* integer for counts / APPROX */
} TreToken;

/* Constructors used by the Lime grammar actions. */
extern RegexAst *regex_ast_literal (TreParseCtx *ctx, int32 cp);
extern RegexAst *regex_ast_any    (TreParseCtx *ctx);
extern RegexAst *regex_ast_anchor (TreParseCtx *ctx, RegexAnchor which);
extern RegexAst *regex_ast_concat (TreParseCtx *ctx, RegexAst *l, RegexAst *r);
extern RegexAst *regex_ast_alt    (TreParseCtx *ctx, RegexAst *l, RegexAst *r);
extern RegexAst *regex_ast_rep    (TreParseCtx *ctx, RegexAst *child,
                                   int32 m, int32 n);
extern RegexAst *regex_ast_approx (TreParseCtx *ctx, RegexAst *child, int32 k);
extern RegexAst *regex_ast_class  (TreParseCtx *ctx, RegexClass *body,
                                   bool negated);
extern RegexClass *regex_ast_class_char (TreParseCtx *ctx, int32 cp);
extern RegexClass *regex_ast_class_range(TreParseCtx *ctx, int32 lo, int32 hi);
extern RegexClass *regex_ast_class_union(TreParseCtx *ctx,
                                         RegexClass *a, RegexClass *b);

/* Top-level entry: parse `pattern` into an AST attached to `ctx->root`.
 * Returns true on success, false on syntax error (ctx->errmsg filled). */
extern bool tre_parse_regex(TreParseCtx *ctx, const char *pattern, int len);

/* ---- Trigram-query tree produced by extract.c ---- */

typedef struct TrigramDisjunct
{
    uint64  trigram_hash;
    int32   min_offset;   /* inclusive lower bound on occurrence offset */
    int32   max_offset;   /* inclusive upper bound; INT32_MAX = unbounded */
} TrigramDisjunct;

typedef struct TrigramConjunct
{
    int32             n;
    TrigramDisjunct  *alts;   /* OR of these; any one must match */
} TrigramConjunct;

typedef struct TrigramQuery
{
    int32             n;       /* 0 means ALL_TIDS (defeated extraction) */
    TrigramConjunct  *conjuncts;   /* AND across all */
    int32             min_match_len;
    int32             global_max_cost;
    bool              always_true;  /* extraction gave up */
} TrigramQuery;

extern bool regex_extract_query(TreParseCtx *ctx, int32 max_cost,
                                TrigramQuery *out);

#endif /* PG_TRE_REGEX_AST_H */
