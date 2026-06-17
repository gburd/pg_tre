/*
 * include/pg_tre/run_catalog.h - run/level catalog iteration
 * (Phase B1, format v7).
 *
 * A pg_tre index is a set of runs.  In format v6 and in the default
 * v7 state there is exactly ONE implicit run rooted at the meta
 * page's root_upper/root_range, so the iterator yields a single run
 * and the scan path is identical to today's.  Later B1 increments
 * populate a real catalog page chain with multiple runs.
 */
#ifndef PG_TRE_RUN_CATALOG_H
#define PG_TRE_RUN_CATALOG_H

#include "postgres.h"

#include "utils/rel.h"

#include "pg_tre/page.h"

/*
 * Iterator over the live runs of an index, newest run_id first.
 * Opaque to callers; constructed by pg_tre_run_catalog_open.
 */
typedef struct PgTreRunIter PgTreRunIter;

/*
 * Open an iterator over the index's live runs.  Reads the meta page
 * to decide between the single-implicit-run case (v6, or v7 with
 * run_catalog_head == InvalidBlockNumber) and a real catalog chain.
 */
extern PgTreRunIter *pg_tre_run_catalog_open(Relation index);

/*
 * Advance the iterator.  Copies the next run into *out and returns
 * true, or returns false when exhausted.  Runs are yielded newest
 * run_id first (so newest-wins merge sees them in order).
 */
extern bool pg_tre_run_catalog_next(PgTreRunIter *it, PgTreRun *out);

/* Release the iterator. */
extern void pg_tre_run_catalog_close(PgTreRunIter *it);

/*
 * Append a new run to the catalog (Phase B1.3).  Allocates and stamps
 * run->run_id from the meta page's monotonic next_run_id, persists the
 * record + meta update atomically (one WAL record, crash-safe), and
 * bumps the live-run count.  Returns the assigned run_id.  Caller must
 * hold a lock excluding concurrent catalog writers.
 */
extern uint64 pg_tre_run_catalog_append(Relation index, PgTreRun *run);

/*
 * Collapse the catalog to a single implicit run (Phase B1.4 adaptive
 * collapse).  Caller has merged all live runs' postings into
 * new_root_upper; this atomically swaps the meta root to it and
 * resets the catalog to single-run state.  WAL-logged.
 */
extern void pg_tre_run_catalog_collapse_reset(Relation index,
                                              BlockNumber new_root_upper,
                                              BlockNumber new_root_range,
                                              uint64 n_trigrams,
                                              uint64 n_tuples);


/*
 * Total live-run count without iterating (reads the meta page).
 * Returns 1 for the single-implicit-run case.
 */
extern uint32 pg_tre_run_count(Relation index);

#endif /* PG_TRE_RUN_CATALOG_H */
