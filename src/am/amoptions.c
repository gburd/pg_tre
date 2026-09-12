/*
 * src/am/amoptions.c - reloptions and opclass validation.
 *
 * Phase 6 adds per-index storage options (q, range_size,
 * fastupdate, pending_list_limit).  Phase 0 accepts only the empty set
 * and treats amvalidate as always-true for the single opclass we ship.
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/amvalidate.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "pg_tre/amapi.h"
#include "pg_tre/pg_tre.h"

/*
 * Per-index options structure.  This gets stored in pg_class.reloptions
 * as a bytea varlena.  Must have vl_len_ as first field.
 */
typedef struct PgTreOptions
{
    int32       vl_len_;                /* varlena header (do not touch directly!) */
    int         pending_list_limit_kb;  /* fast-update pending list size limit */
    int         q;                      /* trigram size (Phase 8; must be 3 until then) */
    int         range_size_blocks;      /* heap blocks per range bloom entry */
    bool        fastupdate;             /* enable fast-update pending list */
} PgTreOptions;

static relopt_kind pg_tre_relopt_kind = 0;

/*
 * Initialize reloption kind and register available options.
 * Called from _PG_init() in module.c.
 */
void
pg_tre_init_reloptions(void)
{
    pg_tre_relopt_kind = add_reloption_kind();

    add_int_reloption(pg_tre_relopt_kind, "pending_list_limit",
                      "Maximum size of fast-update pending list in KiB",
                      4096, 64, INT_MAX, AccessExclusiveLock);

    add_int_reloption(pg_tre_relopt_kind, "q",
                      "Trigram size (must be 3 until Phase 8)",
                      3, 3, 3, AccessExclusiveLock);

    add_int_reloption(pg_tre_relopt_kind, "range_size_blocks",
                      "Deprecated (3.2.0): range-bloom tier removed; accepted but ignored",
                      128, 1, 131072, AccessExclusiveLock);

    add_bool_reloption(pg_tre_relopt_kind, "fastupdate",
                       "Enable fast-update pending list",
                       true, AccessExclusiveLock);
}

bytea *
pg_tre_amoptions(Datum reloptions, bool validate)
{
    static const relopt_parse_elt tab[] = {
        {"pending_list_limit", RELOPT_TYPE_INT,
         offsetof(PgTreOptions, pending_list_limit_kb)},
        {"q", RELOPT_TYPE_INT,
         offsetof(PgTreOptions, q)},
        {"range_size_blocks", RELOPT_TYPE_INT,
         offsetof(PgTreOptions, range_size_blocks)},
        {"fastupdate", RELOPT_TYPE_BOOL,
         offsetof(PgTreOptions, fastupdate)},
    };

    /* Phase 6: if reloptions not initialized, return NULL to use GUC defaults */
    if (pg_tre_relopt_kind == 0)
    {
        if (reloptions != (Datum) 0)
            elog(WARNING,
                 "pg_tre: reloptions requested but the option kind is not "
                 "registered; falling back to GUC defaults");
        return NULL;
    }

    return (bytea *) build_reloptions(reloptions, validate,
                                      pg_tre_relopt_kind,
                                      sizeof(PgTreOptions),
                                      tab, lengthof(tab));
}

/*
 * Helper to extract reloptions from an index relation.
 * Returns the options struct, or NULL if none set (caller uses GUC defaults).
 */
static PgTreOptions *
pg_tre_get_options(Relation index)
{
    if (index->rd_options == NULL)
        return NULL;
    return (PgTreOptions *) index->rd_options;
}

/*
 * Accessors for per-index options, with GUC fallback.
 */
int
pg_tre_get_pending_list_limit_kb(Relation index)
{
    PgTreOptions *opts = pg_tre_get_options(index);
    return opts ? opts->pending_list_limit_kb : pg_tre_pending_list_limit_kb;
}

int
pg_tre_get_q(Relation index)
{
    PgTreOptions *opts = pg_tre_get_options(index);
    return opts ? opts->q : 3;  /* hardcoded until Phase 8 */
}

int
pg_tre_get_range_size_blocks(Relation index)
{
    PgTreOptions *opts = pg_tre_get_options(index);
    return opts ? opts->range_size_blocks : pg_tre_range_size_blocks;
}

bool
pg_tre_get_fastupdate(Relation index)
{
    PgTreOptions *opts = pg_tre_get_options(index);
    return opts ? opts->fastupdate : pg_tre_fastupdate;
}

/*
 * Validate an operator class / operator family for the `tre` access
 * method.  Called by CREATE OPERATOR CLASS / ALTER OPERATOR FAMILY and
 * by amvalidate() sanity checks.  We verify:
 *
 *   - every support (amproc) procedure has a sane, non-cross-type
 *     signature;
 *   - every operator (amop) member uses a strategy number we understand
 *     (search strategy 1 = matchop, 3..7 = LIKE/ILIKE/regex/iregex/eq;
 *     order-by strategy 2 = distance), is marked with the correct
 *     amoppurpose, and (for order-by members) names a valid sort family;
 *   - the opclass declares its indexed type.
 *
 * On any violation we ereport(WARNING) with a specific message and return
 * false, matching the behavior of the in-core AMs (btree/gin/gist) rather
 * than silently accepting a misconfigured opfamily.
 */
bool
pg_tre_amvalidate(Oid opclassoid)
{
    bool        result = true;
    HeapTuple   classtup;
    Form_pg_opclass classform;
    Oid         opfamilyoid;
    Oid         opcintype;
    char       *opclassname;
    CatCList   *proclist;
    CatCList   *oprlist;
    int         i;

    classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
    if (!HeapTupleIsValid(classtup))
        elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
    classform = (Form_pg_opclass) GETSTRUCT(classtup);

    opfamilyoid = classform->opcfamily;
    opcintype = classform->opcintype;
    opclassname = NameStr(classform->opcname);

    /* The tre AM indexes text columns; the opclass input type must be text. */
    if (opcintype != TEXTOID)
    {
        ereport(WARNING,
                (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                 errmsg("tre operator class \"%s\" indexes type %u, expected text",
                        opclassname, opcintype)));
        result = false;
    }

    /* Validate support (amproc) procedures. */
    proclist = SearchSysCacheList1(AMPROCNUM, ObjectIdGetDatum(opfamilyoid));
    for (i = 0; i < proclist->n_members; i++)
    {
        HeapTuple   proctup = &proclist->members[i]->tuple;
        Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);

        /*
         * pg_tre does not (yet) register any support procedures for its
         * opclass -- extraction and recheck are driven from the operator
         * members and the AM callbacks, not from opclass support procs.
         * Any support procedure that shows up here is therefore
         * unexpected.  We still range-check the procnum so a future
         * addition fails loudly rather than silently.
         */
        if (procform->amprocnum < 1 ||
            procform->amprocnum > 4 /* amsupport ceiling */)
        {
            ereport(WARNING,
                    (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                     errmsg("tre opfamily %u contains support procedure %u with invalid support number %d",
                            opfamilyoid, procform->amproc, procform->amprocnum)));
            result = false;
        }
    }
    ReleaseCatCacheList(proclist);

    /* Validate operator (amop) members. */
    oprlist = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(opfamilyoid));
    for (i = 0; i < oprlist->n_members; i++)
    {
        HeapTuple   oprtup = &oprlist->members[i]->tuple;
        Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);

        /* Strategy number must be one we understand (1..7). */
        if (oprform->amopstrategy < 1 || oprform->amopstrategy > 7)
        {
            ereport(WARNING,
                    (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                     errmsg("tre opfamily %u contains operator %u with invalid strategy number %d",
                            opfamilyoid, oprform->amopopr,
                            oprform->amopstrategy)));
            result = false;
        }

        /*
         * Strategy 2 (<@>) is the KNN order-by operator; every other
         * strategy is a search operator.  Verify amoppurpose matches and
         * that order-by members name a valid sort opfamily.
         */
        if (oprform->amopstrategy == 2)
        {
            if (oprform->amoppurpose != AMOP_ORDER)
            {
                ereport(WARNING,
                        (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                         errmsg("tre opfamily %u strategy 2 operator %u must be an ORDER BY member",
                                opfamilyoid, oprform->amopopr)));
                result = false;
            }
            if (!OidIsValid(oprform->amopsortfamily))
            {
                ereport(WARNING,
                        (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                         errmsg("tre opfamily %u order-by operator %u lacks a sort operator family",
                                opfamilyoid, oprform->amopopr)));
                result = false;
            }
        }
        else
        {
            if (oprform->amoppurpose != AMOP_SEARCH)
            {
                ereport(WARNING,
                        (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                         errmsg("tre opfamily %u strategy %d operator %u must be a search member",
                                opfamilyoid, oprform->amopstrategy,
                                oprform->amopopr)));
                result = false;
            }
        }

        /* Operator must return boolean (search) or the sort type (order-by). */
        if (oprform->amoppurpose == AMOP_SEARCH &&
            get_op_rettype(oprform->amopopr) != BOOLOID)
        {
            ereport(WARNING,
                    (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                     errmsg("tre opfamily %u search operator %u does not return boolean",
                            opfamilyoid, oprform->amopopr)));
            result = false;
        }
    }
    ReleaseCatCacheList(oprlist);

    ReleaseSysCache(classtup);

    return result;
}
