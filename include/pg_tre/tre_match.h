/*
 * tre_funcs.h - Interface between PostgreSQL-facing code and TRE.
 *
 * Uses opaque void* for compiled regex handles to avoid exposing
 * TRE types (regex_t, etc.) to translation units that include
 * postgres.h, preventing type conflicts.
 */

#ifndef TRE_FUNCS_H
#define TRE_FUNCS_H

typedef struct TreMatchResult
{
    int matched;        /* 0 = no match, 1 = match */
    int cost;           /* total weighted cost of the match */
    int num_ins;        /* number of insertions */
    int num_del;        /* number of deletions */
    int num_subst;      /* number of substitutions */
    int match_start;    /* byte offset of match start */
    int match_end;      /* byte offset of match end */
} TreMatchResult;

/*
 * Compile a regex pattern. Returns opaque handle on success, NULL on
 * failure (with errcode set to a TRE error code).
 */
void *tre_compile_pattern(const char *pattern, int pattern_len,
                          int *errcode);

/* Free a compiled pattern returned by tre_compile_pattern. */
void tre_free_pattern(void *compiled);

/*
 * Return the number of states in the compiled NFA, or -1 if the handle
 * is NULL.  Callers use this to reject patterns whose automaton is large
 * enough to make matching pathologically slow.
 */
int tre_pattern_num_states(void *compiled);

/*
 * Run approximate matching of a compiled pattern against a string.
 * All cost and limit parameters correspond to TRE's regaparams_t fields.
 */
TreMatchResult tre_do_match(void *compiled, const char *str, int str_len,
                            int max_cost, int cost_ins, int cost_del,
                            int cost_subst, int max_ins, int max_del,
                            int max_subst, int max_err);

/* Return a human-readable error message for a TRE error code. */
const char *tre_errmsg(int errcode);

#endif /* TRE_FUNCS_H */
