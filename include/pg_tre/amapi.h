/*
 * include/pg_tre/amapi.h - access-method handler glue.
 */

#ifndef PG_TRE_AMAPI_H
#define PG_TRE_AMAPI_H

#include "postgres.h"
#include "fmgr.h"
#include "access/amapi.h"

/* Strategy numbers for the text operator class. */
#define PG_TRE_STRATEGY_APPROX_MATCH   1   /* text '%~~' tre_pattern */

/* Support function numbers (custom -- no shared-with-core semantics). */
#define PG_TRE_SUPPORT_EXTRACT_VALUE   1
#define PG_TRE_SUPPORT_EXTRACT_QUERY   2
#define PG_TRE_SUPPORT_MATCH           3
#define PG_TRE_SUPPORT_COMPARE         4

/* Handler entry point, exported to SQL as tre_handler(). */
extern Datum tre_handler(PG_FUNCTION_ARGS);

/* Individual callback prototypes (src/am/...). */
extern IndexBuildResult *pg_tre_ambuild(Relation heap, Relation index,
                                        struct IndexInfo *indexInfo);
extern void pg_tre_ambuildempty(Relation index);
extern bool pg_tre_aminsert(Relation index, Datum *values, bool *isnull,
                            ItemPointer ht_ctid, Relation heapRel,
                            IndexUniqueCheck checkUnique,
                            bool indexUnchanged,
                            struct IndexInfo *indexInfo);
extern IndexBulkDeleteResult *pg_tre_ambulkdelete(IndexVacuumInfo *info,
                                                  IndexBulkDeleteResult *stats,
                                                  IndexBulkDeleteCallback callback,
                                                  void *callback_state);
extern IndexBulkDeleteResult *pg_tre_amvacuumcleanup(IndexVacuumInfo *info,
                                                     IndexBulkDeleteResult *stats);
extern IndexScanDesc pg_tre_ambeginscan(Relation index, int nkeys, int norderbys);
extern void pg_tre_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                            ScanKey orderbys, int norderbys);
extern int64 pg_tre_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern void pg_tre_amendscan(IndexScanDesc scan);
extern void pg_tre_amcostestimate(struct PlannerInfo *root,
                                  struct IndexPath *path,
                                  double loop_count,
                                  Cost *indexStartupCost,
                                  Cost *indexTotalCost,
                                  Selectivity *indexSelectivity,
                                  double *indexCorrelation,
                                  double *indexPages);
extern bytea *pg_tre_amoptions(Datum reloptions, bool validate);
extern bool pg_tre_amvalidate(Oid opclassoid);

#endif /* PG_TRE_AMAPI_H */

/* Accessors for TrePattern, consumed by src/am/amscan.c.  Bodies live
 * in src/util/type_pattern.c.  We declare them here (rather than in
 * a new header) to avoid pulling varlena internals into amapi.h. */
struct TrePatternData;
extern char  *tre_pattern_get_text(struct TrePatternData *p, int *len_out);
extern int32  tre_pattern_get_max_cost(struct TrePatternData *p);
