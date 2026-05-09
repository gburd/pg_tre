/*
** src/query/tre_grammar.y - POSIX regex + TRE approximate-match grammar.
**
** Phase 3 target: produce a bare AST for later analysis by extract.c.
** Lime generates src/query/tre_grammar.c and tre_grammar.h from this
** file.  The generator is built from vendor/lime/lime.c.
**
** The Lime parser is pure LALR(1) -- regex is not technically LALR(1)
** because of look-ahead needs inside character classes, so the
** tokenizer (src/query/tokens.c, written by hand) provides that
** mode-sensitive lexing and the grammar stays context-free.
*/

%include {
#include "postgres.h"
#include "pg_tre/regex_ast.h"          /* Phase 2 */
}

%name           tre_parse
%token_type     { TreToken }
%extra_argument { TreParseCtx *ctx }
%default_type   { RegexAst * }

%type classbody { RegexClass * }
%type classitem { RegexClass * }

%left  PIPE.
%left  CONCAT.
%right STAR PLUS QUESTION LBRACE APPROX.

pattern(A)       ::= alternation(B).                             { ctx->root = B; A = B; }

alternation(A)   ::= concatenation(B).                           { A = B; }
alternation(A)   ::= alternation(B) PIPE concatenation(C).       { A = regex_ast_alt(ctx, B, C); }

concatenation(A) ::= quantified(B).                              { A = B; }
concatenation(A) ::= concatenation(B) quantified(C).             { A = regex_ast_concat(ctx, B, C); }

quantified(A)    ::= atom(B).                                    { A = B; }
quantified(A)    ::= atom(B) STAR.                               { A = regex_ast_rep(ctx, B, 0, -1); }
quantified(A)    ::= atom(B) PLUS.                               { A = regex_ast_rep(ctx, B, 1, -1); }
quantified(A)    ::= atom(B) QUESTION.                           { A = regex_ast_rep(ctx, B,  0,  1); }
quantified(A)    ::= atom(B) LBRACE INT(M) RBRACE.               { A = regex_ast_rep(ctx, B, M.i, M.i); }
quantified(A)    ::= atom(B) LBRACE INT(M) COMMA RBRACE.         { A = regex_ast_rep(ctx, B, M.i, -1); }
quantified(A)    ::= atom(B) LBRACE INT(M) COMMA INT(N) RBRACE.  { A = regex_ast_rep(ctx, B, M.i, N.i); }
quantified(A)    ::= atom(B) APPROX(K).                          { A = regex_ast_approx(ctx, B, K.i); }

atom(A)          ::= LPAREN alternation(B) RPAREN.               { A = B; }
atom(A)          ::= LITERAL(C).                                 { A = regex_ast_literal(ctx, C.cp); }
atom(A)          ::= charclass(B).                               { A = B; }
atom(A)          ::= DOT.                                        { A = regex_ast_any(ctx); }
atom(A)          ::= ANCHOR_START.                               { A = regex_ast_anchor(ctx, REGEX_ANCHOR_START); }
atom(A)          ::= ANCHOR_END.                                 { A = regex_ast_anchor(ctx, REGEX_ANCHOR_END); }

charclass(A)     ::= LBRACKET classbody(B) RBRACKET.             { A = regex_ast_class(ctx, B, false); }
charclass(A)     ::= LBRACKET CARET classbody(B) RBRACKET.       { A = regex_ast_class(ctx, B, true); }

classbody(A)     ::= classitem(B).                               { A = B; }
classbody(A)     ::= classbody(B) classitem(C).                  { A = regex_ast_class_union(ctx, B, C); }

classitem(A)     ::= LITERAL(C).                                 { A = regex_ast_class_char(ctx, C.cp); }
classitem(A)     ::= LITERAL(C1) DASH LITERAL(C2).               { A = regex_ast_class_range(ctx, C1.cp, C2.cp); }

%syntax_error {
    ctx->syntax_error = true;
}
