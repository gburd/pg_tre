/*
 * src/am/amcost.c - planner cost estimation.
 *
 * Phase 6 replaces this with a selectivity-aware estimator that accounts
 * for trigram posting cardinalities, bloom false-positive rates, and
 * recheck cost.  Phase 0 returns a conservative no-op cost that makes
 * the planner prefer seq-scan until a real cost model is in place.
 */

#include "postgres.h"

#include "access/amapi.h"
#include "optimizer/cost.h"
#include "utils/selfuncs.h"

#include "pg_tre/amapi.h"

void
pg_tre_amcostestimate(struct PlannerInfo *root, struct IndexPath *path,
                      double loop_count,
                      Cost *indexStartupCost, Cost *indexTotalCost,
                      Selectivity *indexSelectivity,
                      double *indexCorrelation, double *indexPages)
{
    /* Conservative: force seqscan to win until Phase 6. */
    *indexStartupCost  = disable_cost;
    *indexTotalCost    = disable_cost;
    *indexSelectivity  = 1.0;
    *indexCorrelation  = 0.0;
    *indexPages        = 1.0;
}
