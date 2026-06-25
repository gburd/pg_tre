/*
 * src/am/amoptions.c - reloptions and opclass validation.
 *
 * Phase 6 adds per-index storage options (q, range_size,
 * fastupdate, pending_list_limit).  Phase 0 accepts only the empty set
 * and treats amvalidate as always-true for the single opclass we ship.
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"

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
                      "Heap blocks summarized per range bloom entry",
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
            elog(WARNING, "pg_tre: reloptions requested but not initialized (need shared_preload_libraries)");
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

bool
pg_tre_amvalidate(Oid opclassoid)
{
    /* Phase 6: basic validation stub.  Full validation deferred to Phase 7. */
    return true;
}
