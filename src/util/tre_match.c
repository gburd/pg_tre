/*
 * src/util/tre_match.c - TRE wrapper implementation.
 *
 * This file includes TRE headers but NOT postgres.h. All TRE memory
 * management uses standard malloc/free; callers palloc-wrap results.
 */

#include <stdlib.h>
#include <string.h>

#include "tre.h"
#include "tre-internal.h"
#include "pg_tre/tre_match.h"

void *
tre_compile_pattern(const char *pattern, int pattern_len, int *errcode_out)
{
    regex_t *preg;

    preg = malloc(sizeof(regex_t));
    if (preg == NULL)
    {
        *errcode_out = REG_ESPACE;
        return NULL;
    }
    memset(preg, 0, sizeof(regex_t));

    *errcode_out = tre_regncomp(preg, pattern, (size_t) pattern_len,
                                REG_EXTENDED);
    if (*errcode_out != REG_OK)
    {
        free(preg);
        return NULL;
    }

    return preg;
}

void
tre_free_pattern(void *compiled)
{
    if (compiled == NULL)
        return;
    tre_regfree((regex_t *) compiled);
    free(compiled);
}

/*
 * Return the number of states in the compiled NFA, or -1 if the handle
 * is NULL or carries no internal NFA.  Used by the caller to reject
 * patterns whose compiled automaton is large enough to make matching
 * pathologically slow (the per-string match cost is roughly
 * O(num_states * string_len * max_cost)).
 */
int
tre_pattern_num_states(void *compiled)
{
    regex_t *preg = (regex_t *) compiled;
    tre_tnfa_t *tnfa;

    if (preg == NULL || preg->value == NULL)
        return -1;
    tnfa = (tre_tnfa_t *) preg->value;
    return tnfa->num_states;
}

TreMatchResult
tre_do_match(void *compiled, const char *str, int str_len,
             int max_cost, int cost_ins, int cost_del,
             int cost_subst, int max_ins, int max_del,
             int max_subst, int max_err)
{
    regex_t        *preg = (regex_t *) compiled;
    regaparams_t    params;
    regmatch_t      pmatch;
    regamatch_t     amatch;
    TreMatchResult  result;
    int             ret;

    memset(&result, 0, sizeof(result));

    tre_regaparams_default(&params);
    params.cost_ins  = cost_ins;
    params.cost_del  = cost_del;
    params.cost_subst = cost_subst;
    params.max_cost  = max_cost;
    params.max_ins   = max_ins;
    params.max_del   = max_del;
    params.max_subst = max_subst;
    params.max_err   = max_err;

    /*
     * Bound the edit-distance search space by max_cost.  Callers pass
     * INT_MAX for the individual max_ins/max_del/max_subst/max_err
     * limits, relying on max_cost alone; but leaving them at INT_MAX
     * makes TRE allocate and explore a far larger reach space than the
     * cost ceiling can ever accept.  When max_cost is finite and edit
     * operations have unit (>=1) cost, no individual edit count can
     * exceed max_cost, so clamp the per-operation limits down to it.
     * This is the practical DoS bound on match work: with the NFA-state
     * cap (pattern_cache.c) and this cost clamp, per-string match cost
     * is O(num_states * str_len * max_cost), all three bounded.
     */
    if (max_cost >= 0 && max_cost != INT_MAX)
    {
        if (params.max_err > max_cost)   params.max_err = max_cost;
        if (params.max_ins > max_cost)   params.max_ins = max_cost;
        if (params.max_del > max_cost)   params.max_del = max_cost;
        if (params.max_subst > max_cost) params.max_subst = max_cost;
    }

    memset(&amatch, 0, sizeof(amatch));
    amatch.nmatch = 1;
    amatch.pmatch = &pmatch;

    ret = tre_reganexec(preg, str, (size_t) str_len,
                        &amatch, params, 0);
    if (ret == REG_OK)
    {
        result.matched     = 1;
        result.cost        = amatch.cost;
        result.num_ins     = amatch.num_ins;
        result.num_del     = amatch.num_del;
        result.num_subst   = amatch.num_subst;
        result.match_start = (int) pmatch.rm_so;
        result.match_end   = (int) pmatch.rm_eo;
    }

    return result;
}

const char *
tre_errmsg(int errcode_val)
{
    static char buf[256];

    tre_regerror(errcode_val, NULL, buf, sizeof(buf));
    return buf;
}
