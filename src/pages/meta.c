/*
 * src/pages/meta.c - meta page reader/writer.
 *
 * Phase 1: meta page initialization, read/write, metapage lock helpers.
 *
 * The meta page is always block 0 of the index.  Opening an index
 * first reads the meta page, validates magic + format_version, and
 * caches relevant fields in the relcache entry's rd_amcache slot.
 */

#include "postgres.h"

#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"

/* Phase 1 bodies land here. */
