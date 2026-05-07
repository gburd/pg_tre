/*
 * src/query/tokens.c - hand-written regex tokenizer.
 *
 * Lime handles the grammar (LALR(1)); this file emits tokens the
 * grammar consumes.  Regex requires mode-sensitive lexing (inside
 * [...] a '-' is a range operator, outside it's a literal; inside
 * {~m} the contents are a count) so a mode-free generated tokenizer
 * is not a great fit -- hence the hand roll.
 */

#include "postgres.h"

#include "pg_tre/pg_tre.h"

/* Phase 2 bodies land here. */
