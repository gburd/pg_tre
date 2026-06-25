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
#define PG_TRE_STRATEGY_DISTANCE       2   /* text '<@>' tre_pattern (ORDER BY) */
/* Phase A / A1: pg_trgm-parity operators with a text RHS, lowered
 * to the same trigram engine at k=0; executor rechecks with the
 * built-in operator so the index is a lossy candidate filter. */
#define PG_TRE_STRATEGY_LIKE           3   /* text '~~'  text  (LIKE) */
#define PG_TRE_STRATEGY_ILIKE          4   /* text '~~*' text  (ILIKE) */
#define PG_TRE_STRATEGY_REGEX          5   /* text '~'   text  (regex) */
#define PG_TRE_STRATEGY_IREGEX         6   /* text '~*'  text  (iregex) */
#define PG_TRE_STRATEGY_EQUAL          7   /* text '='   text  (equality) */

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
extern bool  pg_tre_amgettuple(IndexScanDesc scan, ScanDirection dir);
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

/* Initialization. */
extern void pg_tre_init_reloptions(void);

/* Per-index option getters (read opts or fall back to GUC defaults). */
extern int  pg_tre_get_pending_list_limit_kb(Relation index);
extern int  pg_tre_get_q(Relation index);
extern int  pg_tre_get_range_size_blocks(Relation index);
extern bool pg_tre_get_fastupdate(Relation index);

#endif /* PG_TRE_AMAPI_H */

/* Accessors for TrePattern, consumed by src/am/amscan.c.  Bodies live
 * in src/util/type_pattern.c.  We declare them here (rather than in
 * a new header) to avoid pulling varlena internals into amapi.h. */
struct TrePatternData;
extern char  *tre_pattern_get_text(struct TrePatternData *p, int *len_out);
extern int32  tre_pattern_get_max_cost(struct TrePatternData *p);
