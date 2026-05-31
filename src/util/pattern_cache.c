/*
 * tre_cache.c - Per-session LRU cache of compiled TRE regex patterns.
 *
 * Compiled patterns persist across queries in a session.  Fixed capacity
 * with LRU eviction.  Pattern strings are palloc'd in TopMemoryContext;
 * compiled regex handles are malloc'd by TRE and freed via tre_free_pattern.
 */

#include "postgres.h"

#include "miscadmin.h"
#include "utils/memutils.h"

#include "pg_tre/pattern_cache.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/tre_match.h"

#define TRE_CACHE_SLOTS 32

/*
 * Hard ceiling on the regex pattern length we will even attempt to
 * compile.  A pattern longer than this is almost certainly an attack or
 * a mistake; rejecting it early bounds parser-stack and NFA-compile
 * cost before tre_compile_pattern runs.
 */
#define TRE_MAX_PATTERN_LEN (64 * 1024)

typedef struct TreCacheSlot
{
    char   *pattern;        /* palloc'd copy in TopMemoryContext */
    int     pattern_len;
    void   *compiled;       /* opaque handle from tre_compile_pattern */
    uint64  last_used;
} TreCacheSlot;

static TreCacheSlot *cache_slots = NULL;
static uint64 cache_clock = 0;

void
tre_cache_init(void)
{
    MemoryContext oldctx;

    if (cache_slots != NULL)
        return;

    oldctx = MemoryContextSwitchTo(TopMemoryContext);
    cache_slots = palloc0(sizeof(TreCacheSlot) * TRE_CACHE_SLOTS);
    MemoryContextSwitchTo(oldctx);
}

static void
evict_slot(TreCacheSlot *slot)
{
    if (slot->pattern != NULL)
    {
        pfree(slot->pattern);
        slot->pattern = NULL;
    }
    if (slot->compiled != NULL)
    {
        tre_free_pattern(slot->compiled);
        slot->compiled = NULL;
    }
    slot->pattern_len = 0;
    slot->last_used = 0;
}

void *
tre_cache_lookup(const char *pattern, int pattern_len)
{
    int             i;
    TreCacheSlot   *target;
    uint64          min_used;
    int             tre_err;
    void           *compiled;
    MemoryContext   oldctx;

    tre_cache_init();

    if (pattern_len < 0 || pattern_len > TRE_MAX_PATTERN_LEN)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("pg_tre: regex pattern length %d exceeds maximum %d",
                        pattern_len, TRE_MAX_PATTERN_LEN)));

    /* Search existing entries */
    for (i = 0; i < TRE_CACHE_SLOTS; i++)
    {
        TreCacheSlot *slot = &cache_slots[i];

        if (slot->compiled == NULL)
            continue;
        if (slot->pattern_len == pattern_len &&
            memcmp(slot->pattern, pattern, pattern_len) == 0)
        {
            slot->last_used = ++cache_clock;
            return slot->compiled;
        }
    }

    /* Cache miss: compile pattern */
    compiled = tre_compile_pattern(pattern, pattern_len, &tre_err);
    if (compiled == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
                 errmsg("invalid regular expression: %s",
                        tre_errmsg(tre_err))));

    /*
     * Reject patterns whose compiled automaton is large enough to make
     * matching pathologically slow.  pg_tre.max_nfa_states is the
     * documented DoS guardrail; enforce it here, before the pattern can
     * ever reach the match path or be cached.  Per-string match cost is
     * roughly O(num_states * string_len * max_cost), so an unbounded
     * state count is the primary lever an attacker has.
     */
    {
        int nstates = tre_pattern_num_states(compiled);

        if (nstates > pg_tre_max_nfa_states)
        {
            tre_free_pattern(compiled);
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("pg_tre: regex compiles to %d NFA states, "
                            "exceeding pg_tre.max_nfa_states = %d",
                            nstates, pg_tre_max_nfa_states),
                     errhint("Simplify the pattern or raise "
                             "pg_tre.max_nfa_states.")));
        }
    }

    /* Find an empty slot, or the LRU slot for eviction */
    target = &cache_slots[0];
    min_used = cache_slots[0].last_used;
    for (i = 0; i < TRE_CACHE_SLOTS; i++)
    {
        if (cache_slots[i].compiled == NULL)
        {
            target = &cache_slots[i];
            break;
        }
        if (cache_slots[i].last_used < min_used)
        {
            min_used = cache_slots[i].last_used;
            target = &cache_slots[i];
        }
    }

    evict_slot(target);

    /* Store the new entry */
    oldctx = MemoryContextSwitchTo(TopMemoryContext);
    target->pattern = palloc(pattern_len + 1);
    memcpy(target->pattern, pattern, pattern_len);
    target->pattern[pattern_len] = '\0';
    MemoryContextSwitchTo(oldctx);

    target->pattern_len = pattern_len;
    target->compiled = compiled;
    target->last_used = ++cache_clock;

    return compiled;
}
