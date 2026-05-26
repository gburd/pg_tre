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
 *
 * History:
 *   v1     - initial format with byte-based trigrams.
 *   v2     - codepoint-based trigrams (Phase 3.5).
 *   v3     - multi-leaf posting trees (Phase 4.2).
 *   v4     - introduced in 1.4.1.  v3 and v4 are byte-compatible
 *            today; the bump exists so the in-place upgrade machinery
 *            (pg_tre_upgrade_index) has a target version to walk to
 *            once a real format change lands.
 *
 * BREAKING CHANGE: indexes built with v2 or earlier must be REINDEXed.
 *
 * Two-version reader policy: any version in [PG_TRE_FORMAT_VERSION_MIN,
 * PG_TRE_FORMAT_VERSION_LATEST] is readable on the page-decode side.
 * The meta page tracks min_page_format_version across all pages of an
 * index; pg_tre_upgrade_index() rewrites pages forward and bumps that
 * field when every page is at LATEST.  See doc/onpage_format.md.
 */
#define PG_TRE_FORMAT_VERSION_LATEST 5
#define PG_TRE_FORMAT_VERSION_MIN    3

/* Back-compat alias: PG_TRE_FORMAT_VERSION continues to mean "the
 * version we initialise meta pages with at create time".  All new
 * pages -- including pages rewritten by the in-place upgrade -- use
 * PG_TRE_FORMAT_VERSION_LATEST.
 */
#define PG_TRE_FORMAT_VERSION PG_TRE_FORMAT_VERSION_LATEST

/* String version returned by tre_version(). */
#define PG_TRE_VERSION_STRING "pg_tre 1.4.1"

/* Module GUCs, defined in src/module.c. */
extern int  pg_tre_default_max_cost;
extern int  pg_tre_pending_list_limit_kb;
extern int  pg_tre_min_trigram_freq;
extern int  pg_tre_range_size_blocks;
extern int  pg_tre_bloom_tuple_bits;
extern int  pg_tre_max_extraction_fanout;
extern int  pg_tre_max_nfa_states;
extern int  pg_tre_compile_timeout_ms;
extern int  pg_tre_match_timeout_ms;
extern bool pg_tre_fastupdate;
extern bool pg_tre_tuple_bloom_enable;
extern int  pg_tre_tier3_max_candidates;

/* Initialization entry points. */
extern void pg_tre_init_guc(void);
extern void pg_tre_init_rmgr(void);

/* Legacy UDF exports (for internal use). */
extern Datum pg_tre_amatch(PG_FUNCTION_ARGS);
extern Datum pg_tre_amatch_cost(PG_FUNCTION_ARGS);
extern Datum pg_tre_amatch_with_costs(PG_FUNCTION_ARGS);
extern Datum pg_tre_amatch_detail(PG_FUNCTION_ARGS);

#endif /* PG_TRE_H */
