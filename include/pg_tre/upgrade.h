/*
 * include/pg_tre/upgrade.h - in-place index format upgrade.
 *
 * Provides the C entry points behind the SQL functions
 *
 *    pg_tre_upgrade_index(idx regclass)
 *    pg_tre_index_format_status(idx regclass)
 *    pg_tre_index_min_format_version(idx regclass)
 *
 * declared in sql/pg_tre--*.sql.  See doc/onpage_format.md for the
 * narrative description of the upgrade machinery.
 */

#ifndef PG_TRE_UPGRADE_H
#define PG_TRE_UPGRADE_H

#include "postgres.h"
#include "fmgr.h"

extern Datum pg_tre_upgrade_index(PG_FUNCTION_ARGS);
extern Datum pg_tre_index_format_status(PG_FUNCTION_ARGS);
extern Datum pg_tre_index_min_format_version(PG_FUNCTION_ARGS);

#endif /* PG_TRE_UPGRADE_H */
