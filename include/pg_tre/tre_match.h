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
    int timed_out;      /* 1 if the progress hook aborted the match */
} TreMatchResult;

/*
 * Progress hook used to enforce wall-clock match timeouts.
 *
 * tre_match.c MUST NOT include postgres.h (it links against the vendored
 * TRE library and uses raw malloc), so the timeout decision is delegated
 * to a plain-C callback installed by the PostgreSQL-facing layer
 * (src/module.c).  The matcher's per-position NFA loop invokes the hook
 * periodically; a nonzero return aborts the match cleanly and surfaces as
 * TreMatchResult.timed_out = 1.
 *
 * The callback takes no arguments and returns nonzero to abort.  It must
 * be cheap (it is called roughly once per input character) and must not
 * throw / longjmp -- it only signals; the abort is handled by returning
 * an error status up through TRE.
 */
typedef int (*TreProgressHook) (void);

/*
 * Install (or, with NULL, clear) the progress hook.  Returns the previous
 * hook so callers can save/restore around nested matches.  Not thread
 * safe; pg_tre runs single-threaded per backend.
 */
TreProgressHook tre_set_progress_hook(TreProgressHook hook);

/*
 * Invoked from inside the vendored TRE matcher's per-position loop.
 * Returns nonzero when the installed hook requests an abort.  Defined in
 * tre_match.c (postgres.h-free) so the vendored matcher only references a
 * plain extern symbol -- no PG types leak into vendor/tre.
 */
int tre_progress_check(void);

/*
 * Compile-path analogue of the two functions above.  tre_compile.c calls
 * tre_compile_progress_check() from inside its AST-expansion loops so a
 * wall-clock compile deadline (pg_tre.compile_timeout_ms) can interrupt a
 * pathological bounded-repetition blowup before it consumes the backend.
 */
TreProgressHook tre_set_compile_progress_hook(TreProgressHook hook);
int tre_compile_progress_check(void);

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
