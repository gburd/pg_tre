/*
 * src/am/amapi.c - IndexAmRoutine handler for access method `tre`.
 *
 * Registers the pg_tre index access method.  Callback bodies live in
 * phase-specific files (ambuild.c, aminsert.c, amscan.c, amvacuum.c,
 * amcost.c, amoptions.c).  Phases 2+ implement them; earlier phases
 * wire up only what they need and the remaining callbacks raise a
 * clear "not implemented" error so partial progress never silently
 * produces wrong answers.
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/reloptions.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "fmgr.h"
#include "utils/elog.h"
#include "utils/selfuncs.h"

#include "pg_tre/amapi.h"

PG_FUNCTION_INFO_V1(tre_handler);

Datum
tre_handler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

    amroutine->amstrategies    = 7;
    amroutine->amsupport       = 4;
    amroutine->amoptsprocnum   = 0;

    amroutine->amcanorder      = false;
    /*
     * KNN-style ordering: the planner can ask the AM to return rows
     * in ascending order of an indexed-side ordering operator (here,
     * tre_text_ops strategy 2 = `<@>`).  See pg_tre_amgettuple in
     * src/am/amscan.c for the streaming top-k implementation.
     */
    amroutine->amcanorderbyop  = true;
#if PG_VERSION_NUM >= 180000
    /* New in PG18: hash/equality/ordering descriptors. */
    amroutine->amcanhash       = false;
    amroutine->amconsistentequality = false;
    amroutine->amconsistentordering = false;
#endif
    amroutine->amcanbackward   = false;
    amroutine->amcanunique     = false;
    amroutine->amcanmulticol   = false;
    amroutine->amoptionalkey   = false;
    amroutine->amsearcharray   = false;
    amroutine->amsearchnulls   = false;
    amroutine->amstorage       = false;
    amroutine->amclusterable   = false;
    amroutine->ampredlocks     = false;
    amroutine->amcanparallel   = false;       /* enabled in Phase 8 */
    amroutine->amcanbuildparallel = false;    /* enabled in Phase 2 */
    amroutine->amcaninclude    = false;
    amroutine->amusemaintenanceworkmem = true;
    amroutine->amsummarizing   = false;
    amroutine->amparallelvacuumoptions =
        VACUUM_OPTION_PARALLEL_BULKDEL | VACUUM_OPTION_PARALLEL_CLEANUP;
    amroutine->amkeytype       = InvalidOid;

    amroutine->ambuild          = pg_tre_ambuild;
    amroutine->ambuildempty     = pg_tre_ambuildempty;
    amroutine->aminsert         = pg_tre_aminsert;
    amroutine->aminsertcleanup  = NULL;
    amroutine->ambulkdelete     = pg_tre_ambulkdelete;
    amroutine->amvacuumcleanup  = pg_tre_amvacuumcleanup;
    amroutine->amcanreturn      = NULL;        /* lossy */
    amroutine->amcostestimate   = pg_tre_amcostestimate;
#if PG_VERSION_NUM >= 180000
    /* New in PG18: optional callback returning the index tree height. */
    amroutine->amgettreeheight  = NULL;
#endif
    amroutine->amoptions        = pg_tre_amoptions;
    amroutine->amproperty       = NULL;
    amroutine->ambuildphasename = NULL;
    amroutine->amvalidate       = pg_tre_amvalidate;
    amroutine->amadjustmembers  = NULL;
    amroutine->ambeginscan      = pg_tre_ambeginscan;
    amroutine->amrescan         = pg_tre_amrescan;
    amroutine->amgettuple       = pg_tre_amgettuple;
    amroutine->amgetbitmap      = pg_tre_amgetbitmap;
    amroutine->amendscan        = pg_tre_amendscan;
    amroutine->ammarkpos        = NULL;
    amroutine->amrestrpos       = NULL;
    amroutine->amestimateparallelscan = NULL;
    amroutine->aminitparallelscan     = NULL;
    amroutine->amparallelrescan       = NULL;
#if PG_VERSION_NUM >= 180000
    /* New in PG18: strategy/cmptype translation API. */
    amroutine->amtranslatestrategy    = NULL;
    amroutine->amtranslatecmptype     = NULL;
#endif

    PG_RETURN_POINTER(amroutine);
}
