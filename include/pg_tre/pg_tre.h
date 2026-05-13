/*
 * include/pg_tre/pg_tre.h - public header for the pg_tre extension.
 *
 * Collects version macros, module-level GUCs, and forward declarations
 * shared across the extension's translation units.
 */

#ifndef PG_TRE_H
#define PG_TRE_H

#include "postgres.h"
#include "fmgr.h"

/* On-disk format version advertised by meta page.
 * Version 3: multi-leaf posting trees with Lehman-Yao right-links (Phase 4.2).
 * Version 2: codepoint-based trigrams (Phase 3.5).
 * BREAKING CHANGE: indexes built with v1 (byte trigrams) or v2 (single-leaf only)
 * must be REINDEXed after upgrading to v3.
 */
#define PG_TRE_FORMAT_VERSION 3

/* String version returned by tre_version(). */
#define PG_TRE_VERSION_STRING "pg_tre 1.0.0-dev"

/* Module GUCs, defined in src/module.c. */
extern int  pg_tre_default_max_cost;
extern int  pg_tre_pending_list_limit_kb;
extern int  pg_tre_range_size_blocks;
extern int  pg_tre_bloom_tuple_bits;
extern int  pg_tre_max_extraction_fanout;
extern int  pg_tre_max_nfa_states;
extern int  pg_tre_compile_timeout_ms;
extern int  pg_tre_match_timeout_ms;
extern bool pg_tre_fastupdate;
extern bool pg_tre_tuple_bloom_enable;

/* Initialization entry points. */
extern void pg_tre_init_guc(void);
extern void pg_tre_init_rmgr(void);

/* Legacy UDF exports (for internal use). */
extern Datum pg_tre_amatch(PG_FUNCTION_ARGS);
extern Datum pg_tre_amatch_cost(PG_FUNCTION_ARGS);
extern Datum pg_tre_amatch_with_costs(PG_FUNCTION_ARGS);
extern Datum pg_tre_amatch_detail(PG_FUNCTION_ARGS);

#endif /* PG_TRE_H */
