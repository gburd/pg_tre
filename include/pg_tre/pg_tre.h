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

#include "pg_tre/tre_match.h"  /* for TreMatchResult */

/* On-disk format version advertised by meta page.
 *
 * History:
 *   v1     - initial format with byte-based trigrams.
 *   v2     - codepoint-based trigrams (Phase 3.5).
 *   v3     - multi-leaf posting trees (Phase 4.2).
 *   v4     - introduced in 1.5.0.  v3 and v4 are byte-compatible
 *            today; the bump exists so the in-place upgrade machinery
 *            (pg_tre_upgrade_index) has a target version to walk to
 *            once a real format change lands.
 *   v5     - multi-leaf range tier (Phase 8).  Range pages now carry
 *            a PgTreRangeHeader at the start of their content area
 *            with a right_link to the next range page in the chain.
 *            Only RANGE pages differ between v4 and v5; other page
 *            kinds are byte-identical.  Readers handle v<5 range
 *            pages (no header) for back-compat with 1.4.x indexes.
 *   v6     - introduced in 1.6.1.  The vendored sparsemap was updated
 *            to 4.0.0, whose serialized "wire" format widened the
 *            per-chunk start offset from 32 to 64 bits (sparsemap
 *            SM_WIRE_VERSION 1 -> 2).  This fixes silent DATA LOSS for
 *            sparsemap indices >= 2^32 -- which pg_tre reaches on any
 *            heap larger than ~512 MB, because TIDs are packed as
 *            (block << 16) | offset.  The new sparsemap deliberately
 *            cannot decode wire-version-1 blobs, so every page kind
 *            that embeds a serialized sparsemap (posting leaves,
 *            inline upper entries, range blooms) is format-
 *            incompatible with v<6.
 *
 * BREAKING CHANGE: indexes built with v2 or earlier must be REINDEXed.
 * BREAKING CHANGE: indexes built with v5 or earlier (pg_tre < 1.6)
 *   must be REINDEXed -- the sparsemap 4.0.0 wire format is not
 *   backward-readable, and pre-1.6 indexes on large heaps may already
 *   have lost data to the 32-bit-offset bug.  pg_tre_read() rejects
 *   pre-v6 pages with a REINDEX hint rather than returning wrong rows.
 *
 * Reader policy: any version in [PG_TRE_FORMAT_VERSION_MIN,
 * PG_TRE_FORMAT_VERSION_LATEST] is readable on the page-decode side.
 * As of 1.6.1 MIN == LATEST == 6: there is no in-place upgrade from
 * v<6 because the embedded sparsemap bytes are unreadable; REINDEX is
 * the migration (and the data-loss remedy).  See doc/onpage_format.md.
 */
#define PG_TRE_FORMAT_VERSION_LATEST 6
#define PG_TRE_FORMAT_VERSION_MIN    6

/* Back-compat alias: PG_TRE_FORMAT_VERSION continues to mean "the
 * version we initialise meta pages with at create time".  All new
 * pages -- including pages rewritten by the in-place upgrade -- use
 * PG_TRE_FORMAT_VERSION_LATEST.
 */
#define PG_TRE_FORMAT_VERSION PG_TRE_FORMAT_VERSION_LATEST

/* String version returned by tre_version(). */
#define PG_TRE_VERSION_STRING "pg_tre 1.6.1"

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

/*
 * Match-timeout enforcement (defined in src/module.c).
 *
 * pg_tre_arm_match_deadline() installs a progress hook in the TRE
 * matcher and arms a wall-clock deadline of pg_tre_match_timeout_ms
 * milliseconds from now (or the supplied override).  Any tre_do_match()
 * call made while the deadline is armed will abort once the deadline is
 * exceeded, returning TreMatchResult.timed_out = 1.  Callers must invoke
 * pg_tre_check_match_timeout() after the match and pair every arm with a
 * pg_tre_disarm_match_deadline() (use PG_TRY/PG_FINALLY around the match
 * loop so the hook is cleared even on error).
 *
 * The legacy UDFs in src/module.c already arm/disarm internally.  The
 * scan path (src/am/amscan.c) must wrap its tre_do_match() loop with
 * these calls -- see that file's match driver.
 */
extern void pg_tre_arm_match_deadline(int timeout_ms);
extern void pg_tre_disarm_match_deadline(void);
extern void pg_tre_check_match_timeout(const TreMatchResult *r);

/*
 * Compile-deadline analogue of the three functions above.  Arm before a
 * tre_compile_pattern() call, disarm in a PG_FINALLY, and call
 * pg_tre_check_compile_timeout() to convert a deadline abort into an
 * ereport(ERROR) distinct from a syntax error.
 */
extern void pg_tre_arm_compile_deadline(int timeout_ms);
extern void pg_tre_disarm_compile_deadline(void);
extern void pg_tre_check_compile_timeout(void);

/* Legacy UDF exports (for internal use). */
extern Datum pg_tre_amatch(PG_FUNCTION_ARGS);
extern Datum pg_tre_amatch_cost(PG_FUNCTION_ARGS);
extern Datum pg_tre_amatch_with_costs(PG_FUNCTION_ARGS);
extern Datum pg_tre_amatch_detail(PG_FUNCTION_ARGS);

#endif /* PG_TRE_H */
