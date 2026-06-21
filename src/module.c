/*
 * src/module.c - pg_tre module initialization.
 *
 * PG_MODULE_MAGIC, _PG_init, GUC registration, rmgr registration, and
 * the legacy tre_amatch* UDFs inherited from the 0.1.0 UDF-only extension.
 * The index AM callbacks live under src/am/.
 */

#include "postgres.h"

#include <limits.h>

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "pg_tre/pg_tre.h"
#include "pg_tre/amapi.h"
#include "pg_tre/tre_match.h"
#include "pg_tre/pattern_cache.h"
#include "pg_tre/xlog.h"

PG_MODULE_MAGIC;

/* ---- GUCs ---- */

int  pg_tre_default_max_cost       = 3;
int  pg_tre_pending_list_limit_kb  = 4096;   /* 4 MiB */
int  pg_tre_min_trigram_freq       = 1;      /* 0 disables */
int  pg_tre_range_size_blocks      = 128;
int  pg_tre_bloom_tuple_bits       = 128;
int  pg_tre_max_extraction_fanout  = 4096;
int  pg_tre_max_nfa_states         = 10000;
int  pg_tre_compile_timeout_ms     = 1000;
int  pg_tre_match_timeout_ms       = 1000;
bool pg_tre_fastupdate             = true;
bool pg_tre_tuple_bloom_enable     = true;  /* lossy positional pre-filter; PGC_USERSET as of 2.0.3 */
bool pg_tre_flush_to_run           = false; /* Phase B1.3: gated (see bench) */
bool pg_tre_crack_on_read          = false; /* Phase B1.5: gated (see bench) */
bool pg_tre_coalesce_enable        = false; /* v2.0 coalescing: gated (see bench) */
int  pg_tre_tier3_max_candidates   = 50000;
int  pg_tre_build_max_entries_mb   = 0;      /* 0 = unlimited (default since 1.8.0) */
double pg_tre_similarity_threshold = 0.3;    /* pg_trgm-compatible %% threshold */

void
pg_tre_init_guc(void)
{
    DefineCustomIntVariable("pg_tre.default_max_cost",
        "Default maximum approximate-match edit cost when unspecified.",
        NULL,
        &pg_tre_default_max_cost,
        3, 0, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.pending_list_limit",
        "Maximum size of the fast-update pending list, in KiB.",
        NULL,
        &pg_tre_pending_list_limit_kb,
        4096, 64, INT_MAX, PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.range_size_blocks",
        "Heap blocks summarized by each range-bloom entry.",
        NULL,
        &pg_tre_range_size_blocks,
        128, 1, 131072, PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.bloom_tuple_bits",
        "Bits per per-tuple bloom filter.",
        NULL,
        &pg_tre_bloom_tuple_bits,
        128, 32, 1024, PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.max_extraction_fanout",
        "Maximum number of trigram disjuncts a query may emit.",
        NULL,
        &pg_tre_max_extraction_fanout,
        4096, 1, 65536, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.max_nfa_states",
        "Reject patterns whose compiled NFA exceeds this state count.",
        NULL,
        &pg_tre_max_nfa_states,
        10000, 32, 1000000, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.compile_timeout_ms",
        "Maximum regex-compile time before aborting (milliseconds).",
        NULL,
        &pg_tre_compile_timeout_ms,
        1000, 1, 600000, PGC_USERSET, GUC_UNIT_MS, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.match_timeout_ms",
        "Per-query match-time budget (milliseconds).",
        NULL,
        &pg_tre_match_timeout_ms,
        1000, 1, 600000, PGC_USERSET, GUC_UNIT_MS, NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_tre.fastupdate",
        "Enable the fast-update pending list for inserts.",
        NULL,
        &pg_tre_fastupdate,
        true, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_tre.tuple_bloom_enable",
        "Store the per-tuple positional payload (positions + bloom) in"
        " posting leaves for the tier-3.1 positional pre-filter.",
        "This payload is a LOSSY pre-filter only -- the executor recheck is"
        " authoritative, so disabling it never changes query results, only"
        " how many candidates are pre-filtered before recheck.  It costs"
        " ~22 bytes per (trigram,TID) UNCOMPRESSED, which on a large/high-"
        " cardinality corpus dominates the index (it was ~58%% of one"
        " 30k-row field-report index).  Now PGC_USERSET so it can be"
        " toggled per session before CREATE INDEX (was PGC_SIGHUP and"
        " uncontrollable per build before 2.0.3).  Default ON for"
        " compatibility; set OFF for a much denser index when the"
        " positional pre-filter is not needed.",
        &pg_tre_tuple_bloom_enable,
        true, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_tre.flush_to_run",
        "Phase B1.3: flush the pending list into a new LSM run instead"
        " of merging it into the single base structure.",
        "When on, pg_tre_pending_merge builds a pending-only run (its own"
        " upper tree + postings over just the newly-ingested TIDs) and"
        " appends it to the run catalog, leaving the base run untouched;"
        " the multi-run scan unions all runs at query time.  When off"
        " (the default), the historical behavior is used: the pending"
        " list is merged into the single base tree.  Runs accrue and are"
        " bounded by Hanoi leveling + adaptive collapse at VACUUM (B1.4)."
        " Off by default: on a single-run index it adds the multi-run"
        " scan path with no measured benefit (the crack/flush payoff"
        " needs multi-run indexes); cost is ~0 on clean hardware (see"
        " bench/RESULTS-v2.0-ab.md).",
        &pg_tre_flush_to_run,
        false, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_tre.crack_on_read",
        "Phase B1.5: coalesce a trigram's postings across runs once per"
        " scan (lazy per-trigram materialization, the cracking payoff).",
        "When on, a multi-run scan materializes each query trigram's"
        " union across all runs at most once and reuses it for every"
        " later touch of the same trigram in the same scan (and for"
        " co-occurring trigrams of the rows touched).  This is a"
        " per-scan, in-memory optimization only: it performs NO disk"
        " write-back, takes no locks, and is a pure no-op on a hot"
        " standby or read-only transaction.  Results are byte-identical"
        " with or without it (the executor still rechecks every row)."
        " The durable run-catalog crack (persisting the coalesced run so"
        " the NEXT scan touches fewer runs) is deferred -- see"
        " doc/specs/phaseB1-run-catalog.md B1.5.  Off by default; only"
        " wins on multi-run or nonselective scans (see bench results).",
        &pg_tre_crack_on_read,
        false, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_tre.coalesce_enable",
        "v2.0: pack medium-cardinality trigram postings onto shared"
        " coalesced pages instead of one dedicated page each.",
        "When on, CREATE INDEX places trigram postings whose serialized"
        " size is between pg_tre.inline threshold and the coalesce cap"
        " onto shared PG_TRE_PAGE_POSTING_COALESCED pages, shrinking the"
        " index's page count.  Results are byte-identical (the executor"
        " rechecks every row); only on-disk size changes.  Format v8"
        " (additive: v6/v7 indexes read unchanged, no REINDEX).  Off by"
        " default: maintenance-safe (VACUUM/merge are coalesced-aware)."
        " The coalesce cap is the >=2-slots-per-page boundary (~4056 B),"
        " so the whole medium band packs 2-3 postings per page instead of"
        " one dedicated 8 KB leaf each -- a measured 1.2x-1.6x index-size"
        " reduction on corpora with a medium-cardinality trigram tail."
        " Adds slot-resolution overhead on read and drops the per-tuple"
        " tier-3 pre-filter for coalesced trigrams (recheck still"
        " authoritative).  See doc/specs/posting-page-coalescing.md and"
        " .agent/notes/blocker1-density-brief.md.",
        &pg_tre_coalesce_enable,
        false, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.tier3_max_candidates",
        "Skip per-tuple bloom and positional filters when the candidate"
        " set already exceeds this many TIDs.",
        "Tier-3 filters do per-TID work proportional to the candidate"
        " cardinality.  For very large candidate sets the recheck path"
        " is cheaper than running tier-3, so we skip it.  Set to 0 to"
        " disable tier-3 entirely; INT_MAX disables this safety belt.",
        &pg_tre_tier3_max_candidates,
        50000, 0, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_tre.build_max_entries_mb",
        "Optional temp-disk safety valve for index builds (0 = unlimited,"
        " the default).",
        "Since 1.8.0 the build sorts trigram tuples with tuplesort, so"
        " peak *memory* is bounded by maintenance_work_mem (spilled to"
        " temp files) regardless of corpus size -- the build no longer"
        " OOMs.  This guard now only bounds the *number* of trigram"
        " tuples (and thus temp-disk use): when the emitted-tuple count"
        " times 24 bytes would exceed this many megabytes the build"
        " fails with ERRCODE_PROGRAM_LIMIT_EXCEEDED instead of filling"
        " the temp tablespace.  Default 0 (disabled) so large but"
        " legitimate builds are not blocked; set it if you want a hard"
        " ceiling on build temp-disk consumption.",
        &pg_tre_build_max_entries_mb,
        0, 0, INT_MAX, PGC_USERSET, GUC_UNIT_MB, NULL, NULL, NULL);

    DefineCustomRealVariable("pg_tre.similarity_threshold",
        "Threshold the %% operator uses for trigram-set similarity.",
        "text %% text is true when tre_trgm_similarity(a, b) >= this"
        " value.  Mirrors pg_trgm.similarity_threshold so queries"
        " port unchanged.",
        &pg_tre_similarity_threshold,
        0.3, 0.0, 1.0, PGC_USERSET, 0, NULL, NULL, NULL);

    /*
     * Cardinality-aware build (1.2.1+).  Posting trees with fewer
     * than this many TIDs are dropped during ambuild.  A trigram
     * appearing in 1-2 rows out of millions isn't a useful
     * candidate-narrowing filter — its posting tree is mostly
     * structural overhead (one 8 KB page per trigram).  Queries
     * matching only such rare trigrams fall back to other trigrams
     * in the same conjunct; if every trigram in the conjunct was
     * dropped, the planner emits a fully-lossy bitmap covering the
     * heap and the executor recheck filters.  Correctness is
     * preserved either way.
     *
     * Default 1 (drop nothing) preserves prior behavior.  Set to 2
     * or higher to drop singleton/doubleton trigrams; 16 is a
     * reasonable starting point for large corpora.  PGC_SIGHUP
     * because changing it mid-build would corrupt the index;
     * effective on the next CREATE INDEX / REINDEX.
     */
    DefineCustomIntVariable("pg_tre.min_trigram_freq",
        "Skip building posting trees for trigrams with fewer TIDs.",
        "Posting trees below this threshold are not persisted, "
        "trading some recheck work for a smaller index.  0 or 1 "
        "disables the optimization (default).",
        &pg_tre_min_trigram_freq,
        1, 0, INT_MAX, PGC_SIGHUP, 0, NULL, NULL, NULL);

    MarkGUCPrefixReserved("pg_tre");
}

/* ====================================================================
 * Match-timeout enforcement.
 *
 * tre_match.c / vendor/tre cannot include postgres.h, so they cannot
 * call GetCurrentTimestamp() or CHECK_FOR_INTERRUPTS().  Instead the
 * matcher's per-position NFA loop calls the plain-C hook installed here.
 * The hook compares the current wall-clock time against a deadline armed
 * by pg_tre_arm_match_deadline() and returns nonzero to abort once the
 * deadline is passed; the abort surfaces as TreMatchResult.timed_out,
 * which pg_tre_check_match_timeout() converts into an ereport(ERROR).
 *
 * Granularity: the hook fires once per input character, so enforcement
 * is exact to within one NFA-position step (sub-millisecond for typical
 * inputs).  GetCurrentTimestamp() is a cheap gettimeofday() wrapper.
 *
 * This is a per-backend, single-threaded mechanism (no re-entrancy):
 * nested matches share one deadline.  Callers must pair every arm with
 * a disarm, preferably via PG_TRY/PG_FINALLY.
 * ==================================================================== */

static TimestampTz pg_tre_match_deadline = 0;     /* 0 == not armed */
static int         pg_tre_match_arm_depth = 0;

/*
 * Progress hook handed to the TRE matcher.  Plain C signature, no
 * arguments, returns nonzero to abort.  Must be cheap and must not throw.
 */
static int
pg_tre_match_progress_hook(void)
{
    if (pg_tre_match_deadline != 0 &&
        GetCurrentTimestamp() >= pg_tre_match_deadline)
        return 1;               /* deadline exceeded: abort the match */
    return 0;
}

/*
 * Arm a wall-clock deadline for subsequent tre_do_match() calls and
 * install the progress hook.  timeout_ms <= 0 means "use the GUC".
 * Re-entrant: nested arms keep the outermost (earliest-disarming) hook
 * installed and tighten the deadline to the soonest one.
 */
void
pg_tre_arm_match_deadline(int timeout_ms)
{
    int          ms = (timeout_ms > 0) ? timeout_ms : pg_tre_match_timeout_ms;
    TimestampTz  deadline;

    if (ms <= 0)
        return;                 /* timeout disabled */

    deadline = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), ms);

    if (pg_tre_match_arm_depth == 0)
    {
        pg_tre_match_deadline = deadline;
        (void) tre_set_progress_hook(pg_tre_match_progress_hook);
    }
    else if (deadline < pg_tre_match_deadline)
    {
        pg_tre_match_deadline = deadline;   /* tighten */
    }
    pg_tre_match_arm_depth++;
}

/* Disarm the deadline; uninstall the hook when the outermost arm exits. */
void
pg_tre_disarm_match_deadline(void)
{
    if (pg_tre_match_arm_depth == 0)
        return;
    if (--pg_tre_match_arm_depth == 0)
    {
        pg_tre_match_deadline = 0;
        (void) tre_set_progress_hook(NULL);
    }
}

/* ====================================================================
 * Compile-timeout enforcement (pg_tre.compile_timeout_ms).
 *
 * Mirrors the match-deadline machinery above but drives the separate
 * compile progress hook (tre_set_compile_progress_hook), which the
 * vendored TRE compiler's AST-expansion loops poll via
 * tre_compile_progress_check().  A pathological bounded-repetition
 * pattern (e.g. a{1000}{1000}{1000}) can otherwise spin the backend for
 * a long time inside tre_regncomp() with no CHECK_FOR_INTERRUPTS reach.
 * ==================================================================== */

static TimestampTz pg_tre_compile_deadline = 0;   /* 0 == not armed */
static int         pg_tre_compile_arm_depth = 0;
static bool        pg_tre_compile_timed_out = false;

static int
pg_tre_compile_progress_hook(void)
{
    if (pg_tre_compile_deadline != 0 &&
        GetCurrentTimestamp() >= pg_tre_compile_deadline)
    {
        pg_tre_compile_timed_out = true;
        return 1;               /* deadline exceeded: abort the compile */
    }
    return 0;
}

/*
 * Arm a wall-clock deadline for a subsequent tre_compile_pattern() call
 * and install the compile progress hook.  timeout_ms <= 0 means "use the
 * GUC".  Re-entrant like the match arm.
 */
void
pg_tre_arm_compile_deadline(int timeout_ms)
{
    int          ms = (timeout_ms > 0) ? timeout_ms : pg_tre_compile_timeout_ms;
    TimestampTz  deadline;

    if (ms <= 0)
        return;                 /* timeout disabled */

    deadline = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), ms);

    if (pg_tre_compile_arm_depth == 0)
    {
        pg_tre_compile_deadline = deadline;
        pg_tre_compile_timed_out = false;
        (void) tre_set_compile_progress_hook(pg_tre_compile_progress_hook);
    }
    else if (deadline < pg_tre_compile_deadline)
    {
        pg_tre_compile_deadline = deadline;   /* tighten */
    }
    pg_tre_compile_arm_depth++;
}

/* Disarm the compile deadline; uninstall the hook at the outermost exit. */
void
pg_tre_disarm_compile_deadline(void)
{
    if (pg_tre_compile_arm_depth == 0)
        return;
    if (--pg_tre_compile_arm_depth == 0)
    {
        pg_tre_compile_deadline = 0;
        (void) tre_set_compile_progress_hook(NULL);
    }
}

/*
 * Raise if the most recent armed compile aborted on the deadline.  Call
 * immediately after tre_compile_pattern() returns NULL while the
 * deadline is (or was just) armed; distinguishes a genuine syntax error
 * from a wall-clock timeout.
 */
void
pg_tre_check_compile_timeout(void)
{
    if (pg_tre_compile_timed_out)
    {
        pg_tre_compile_timed_out = false;
        ereport(ERROR,
                (errcode(ERRCODE_QUERY_CANCELED),
                 errmsg("pg_tre: regex compilation exceeded "
                        "pg_tre.compile_timeout_ms = %d ms",
                        pg_tre_compile_timeout_ms),
                 errhint("Simplify the pattern (reduce nested bounded "
                         "repetitions) or raise pg_tre.compile_timeout_ms "
                         "for trusted callers.")));
    }
}

/*
 * Turn a timed-out match result into an error.  Call immediately after
 * each tre_do_match() while the deadline is armed.
 */
void
pg_tre_check_match_timeout(const TreMatchResult *r)
{
    if (r != NULL && r->timed_out)
        ereport(ERROR,
                (errcode(ERRCODE_QUERY_CANCELED),
                 errmsg("pg_tre: regex match exceeded "
                        "pg_tre.match_timeout_ms = %d ms",
                        pg_tre_match_timeout_ms),
                 errhint("Simplify the pattern, reduce max_cost, or raise "
                         "pg_tre.match_timeout_ms for trusted callers.")));
}

/*
 * Convenience wrapper used by the legacy UDFs: arm the deadline, run one
 * match, disarm (even on error), and raise on timeout.  Keeps the per-UDF
 * boilerplate to a single call.
 */
static TreMatchResult
pg_tre_match_guarded(void *compiled, const char *str, int str_len,
                     int max_cost, int cost_ins, int cost_del,
                     int cost_subst, int max_ins, int max_del,
                     int max_subst, int max_err)
{
    TreMatchResult result;

    pg_tre_arm_match_deadline(0);
    PG_TRY();
    {
        result = tre_do_match(compiled, str, str_len,
                              max_cost, cost_ins, cost_del, cost_subst,
                              max_ins, max_del, max_subst, max_err);
    }
    PG_FINALLY();
    {
        pg_tre_disarm_match_deadline();
    }
    PG_END_TRY();

    pg_tre_check_match_timeout(&result);
    return result;
}

/* ---- rmgr registration ---- */

static RmgrData pg_tre_rmgr = {
    .rm_name          = RM_PG_TRE_NAME,
    .rm_redo          = pg_tre_redo,
    .rm_desc          = pg_tre_desc,
    .rm_identify      = pg_tre_identify,
    .rm_startup       = pg_tre_startup,
    .rm_cleanup       = pg_tre_cleanup,
    .rm_mask          = pg_tre_mask,
    .rm_decode        = NULL,
};

void
pg_tre_init_rmgr(void)
{
    RegisterCustomRmgr(RM_PG_TRE_ID, &pg_tre_rmgr);
}

/* ---- _PG_init ---- */

void _PG_init(void);

void
_PG_init(void)
{
    pg_tre_init_guc();

    /*
     * Register the index reloption kind so per-index storage params
     * (fastupdate, pending_list_limit, q, ...) are recognized.  Must
     * happen at load time; without it pg_tre_relopt_kind stays 0 and
     * every reloption is silently ignored (with a WARNING from
     * amoptions).  Reloptions work for on-demand CREATE EXTENSION too,
     * so this is NOT gated on shared_preload_libraries.
     */
    pg_tre_init_reloptions();

    /*
     * Custom resource managers must be registered during preload.
     * When pg_tre is loaded on demand (CREATE EXTENSION without
     * shared_preload_libraries), skip the rmgr registration: the
     * legacy UDFs still work, but the AM's write path will reject
     * index mutations until a preload-enabled restart.
     */
    if (process_shared_preload_libraries_in_progress)
        pg_tre_init_rmgr();

    tre_cache_init();
}

/* ====================================================================
 * Legacy UDFs preserved from pg_tre 0.1.0.  These keep the UDF-only
 * interface working during the AM transition and will be retained in
 * 1.0.0 as convenience functions.
 * ==================================================================== */

PG_FUNCTION_INFO_V1(pg_tre_amatch);

Datum
pg_tre_amatch(PG_FUNCTION_ARGS)
{
    text   *input    = PG_GETARG_TEXT_PP(0);
    text   *pattern  = PG_GETARG_TEXT_PP(1);
    int32   max_cost = PG_GETARG_INT32(2);
    void   *compiled;
    TreMatchResult result;

    compiled = tre_cache_lookup(VARDATA_ANY(pattern),
                                VARSIZE_ANY_EXHDR(pattern));

    result = pg_tre_match_guarded(compiled,
                          VARDATA_ANY(input),
                          VARSIZE_ANY_EXHDR(input),
                          max_cost, 1, 1, 1,
                          INT_MAX, INT_MAX, INT_MAX, INT_MAX);

    PG_RETURN_BOOL(result.matched != 0);
}

PG_FUNCTION_INFO_V1(pg_tre_amatch_cost);

Datum
pg_tre_amatch_cost(PG_FUNCTION_ARGS)
{
    text   *input    = PG_GETARG_TEXT_PP(0);
    text   *pattern  = PG_GETARG_TEXT_PP(1);
    int32   max_cost = PG_GETARG_INT32(2);
    void   *compiled;
    TreMatchResult result;

    compiled = tre_cache_lookup(VARDATA_ANY(pattern),
                                VARSIZE_ANY_EXHDR(pattern));

    result = pg_tre_match_guarded(compiled,
                          VARDATA_ANY(input),
                          VARSIZE_ANY_EXHDR(input),
                          max_cost, 1, 1, 1,
                          INT_MAX, INT_MAX, INT_MAX, INT_MAX);

    if (!result.matched)
        PG_RETURN_NULL();

    PG_RETURN_INT32(result.cost);
}

PG_FUNCTION_INFO_V1(pg_tre_amatch_with_costs);

Datum
pg_tre_amatch_with_costs(PG_FUNCTION_ARGS)
{
    text   *input      = PG_GETARG_TEXT_PP(0);
    text   *pattern    = PG_GETARG_TEXT_PP(1);
    int32   max_cost   = PG_GETARG_INT32(2);
    int32   cost_ins   = PG_GETARG_INT32(3);
    int32   cost_del   = PG_GETARG_INT32(4);
    int32   cost_subst = PG_GETARG_INT32(5);
    void   *compiled;
    TreMatchResult result;

    compiled = tre_cache_lookup(VARDATA_ANY(pattern),
                                VARSIZE_ANY_EXHDR(pattern));

    result = pg_tre_match_guarded(compiled,
                          VARDATA_ANY(input),
                          VARSIZE_ANY_EXHDR(input),
                          max_cost, cost_ins, cost_del, cost_subst,
                          INT_MAX, INT_MAX, INT_MAX, INT_MAX);

    PG_RETURN_BOOL(result.matched != 0);
}

PG_FUNCTION_INFO_V1(pg_tre_amatch_detail);

Datum
pg_tre_amatch_detail(PG_FUNCTION_ARGS)
{
    ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc       tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext   per_query_ctx;
    MemoryContext   oldcontext;
    text   *input    = PG_GETARG_TEXT_PP(0);
    text   *pattern  = PG_GETARG_TEXT_PP(1);
    int32   max_cost = PG_GETARG_INT32(2);
    void   *compiled;
    TreMatchResult result;

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context "
                        "that cannot accept a set")));
    if (!(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required but not allowed "
                        "in this context")));

    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    oldcontext = MemoryContextSwitchTo(per_query_ctx);

    tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
    tupstore = tuplestore_begin_heap(false, false, work_mem);

    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;

    MemoryContextSwitchTo(oldcontext);

    compiled = tre_cache_lookup(VARDATA_ANY(pattern),
                                VARSIZE_ANY_EXHDR(pattern));

    result = pg_tre_match_guarded(compiled,
                          VARDATA_ANY(input),
                          VARSIZE_ANY_EXHDR(input),
                          max_cost, 1, 1, 1,
                          INT_MAX, INT_MAX, INT_MAX, INT_MAX);

    if (result.matched)
    {
        Datum   values[6];
        bool    nulls[6];

        memset(nulls, 0, sizeof(nulls));
        values[0] = Int32GetDatum(result.cost);
        values[1] = Int32GetDatum(result.num_ins);
        values[2] = Int32GetDatum(result.num_del);
        values[3] = Int32GetDatum(result.num_subst);
        values[4] = Int32GetDatum(result.match_start);
        values[5] = Int32GetDatum(result.match_end);

        tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }

    return (Datum) 0;
}

/*
 * Similarity score derived from approximate-match edit distance.
 *
 * Definition: similarity = 1 - (cost / max(len(input), len(pattern_text)))
 *
 * Range: [0.0, 1.0] where 1.0 means exact match (cost=0) and 0.0 means
 * the input has been entirely rewritten (cost == max length).  Returns
 * 0.0 when the match exceeds max_cost (i.e. no approximate match was
 * found within the budget).  This formulation matches pg_trgm's
 * similarity() in spirit: higher = more similar.
 *
 * For ranking, prefer ORDER BY tre_distance(...) ASC (smaller cost
 * first) over similarity DESC — the integer cost is more compact and
 * does not depend on input length.
 */
static double
tre_compute_similarity(const char *input, int input_len,
                       const char *pattern, int pattern_len,
                       int max_cost)
{
    void           *compiled;
    TreMatchResult  result;
    int             max_len;

    compiled = tre_cache_lookup(pattern, pattern_len);
    result = pg_tre_match_guarded(compiled, input, input_len,
                          max_cost, 1, 1, 1,
                          INT_MAX, INT_MAX, INT_MAX, INT_MAX);

    if (!result.matched)
        return 0.0;

    max_len = input_len > pattern_len ? input_len : pattern_len;
    if (max_len <= 0)
        return 1.0;

    return 1.0 - ((double) result.cost / (double) max_len);
}

PG_FUNCTION_INFO_V1(pg_tre_similarity);

Datum
pg_tre_similarity(PG_FUNCTION_ARGS)
{
    text   *input    = PG_GETARG_TEXT_PP(0);
    text   *pattern  = PG_GETARG_TEXT_PP(1);
    int32   max_cost = PG_GETARG_INT32(2);
    double  sim;

    sim = tre_compute_similarity(VARDATA_ANY(input),
                                  VARSIZE_ANY_EXHDR(input),
                                  VARDATA_ANY(pattern),
                                  VARSIZE_ANY_EXHDR(pattern),
                                  max_cost);

    PG_RETURN_FLOAT8(sim);
}

/*
 * tre_distance(input, pattern, max_cost) -> int
 *
 * Returns the actual edit distance of the best alignment, or NULL if
 * no match exists within max_cost.  This is the integer cost as
 * computed by TRE (with default insertion=deletion=substitution=1).
 *
 * Equivalent to tre_amatch_cost() but with a different name to make
 * the ranking-by-distance idiom obvious in EXPLAIN plans.
 *
 * Use ORDER BY tre_distance(...) ASC LIMIT N to emit the N closest
 * matches.
 */
PG_FUNCTION_INFO_V1(pg_tre_distance);

Datum
pg_tre_distance(PG_FUNCTION_ARGS)
{
    text   *input    = PG_GETARG_TEXT_PP(0);
    text   *pattern  = PG_GETARG_TEXT_PP(1);
    int32   max_cost = PG_GETARG_INT32(2);
    void   *compiled;
    TreMatchResult result;

    compiled = tre_cache_lookup(VARDATA_ANY(pattern),
                                VARSIZE_ANY_EXHDR(pattern));
    result = pg_tre_match_guarded(compiled,
                          VARDATA_ANY(input),
                          VARSIZE_ANY_EXHDR(input),
                          max_cost, 1, 1, 1,
                          INT_MAX, INT_MAX, INT_MAX, INT_MAX);

    if (!result.matched)
        PG_RETURN_NULL();

    PG_RETURN_INT32(result.cost);
}

/*
 * Similarity / distance against the strongly-typed tre_pattern.
 *
 * The pattern carries its own max_cost, so these wrappers don't
 * need a separate argument.  Useful for ORDER BY clauses where
 * repeating tre_pattern('foo', 2) twice would parse the regex
 * twice; the indexed lookup uses the cached compile.
 */
PG_FUNCTION_INFO_V1(pg_tre_similarity_pattern);

Datum
pg_tre_similarity_pattern(PG_FUNCTION_ARGS)
{
    text                  *input    = PG_GETARG_TEXT_PP(0);
    struct TrePatternData *pat      = (struct TrePatternData *)
                                       PG_GETARG_POINTER(1);
    char                  *pat_text;
    int                    pat_len;
    int32                  max_cost;
    double                 sim;

    pat_text = tre_pattern_get_text(pat, &pat_len);
    max_cost = tre_pattern_get_max_cost(pat);

    sim = tre_compute_similarity(VARDATA_ANY(input),
                                  VARSIZE_ANY_EXHDR(input),
                                  pat_text, pat_len,
                                  max_cost);

    PG_RETURN_FLOAT8(sim);
}

PG_FUNCTION_INFO_V1(pg_tre_distance_pattern);

Datum
pg_tre_distance_pattern(PG_FUNCTION_ARGS)
{
    text                  *input    = PG_GETARG_TEXT_PP(0);
    struct TrePatternData *pat      = (struct TrePatternData *)
                                       PG_GETARG_POINTER(1);
    char                  *pat_text;
    int                    pat_len;
    int32                  max_cost;
    void                  *compiled;
    TreMatchResult         result;

    pat_text = tre_pattern_get_text(pat, &pat_len);
    max_cost = tre_pattern_get_max_cost(pat);

    compiled = tre_cache_lookup(pat_text, pat_len);
    result = pg_tre_match_guarded(compiled,
                          VARDATA_ANY(input),
                          VARSIZE_ANY_EXHDR(input),
                          max_cost, 1, 1, 1,
                          INT_MAX, INT_MAX, INT_MAX, INT_MAX);

    if (!result.matched)
        PG_RETURN_NULL();

    PG_RETURN_INT32(result.cost);
}

PG_FUNCTION_INFO_V1(pg_tre_version);

Datum
pg_tre_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text(PG_TRE_VERSION_STRING " (TRE 0.9.0)"));
}
