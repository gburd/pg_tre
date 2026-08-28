/*
 * include/pg_tre/surf_page.h - on-disk persistence for the SuRF filter.
 */

#ifndef PG_TRE_SURF_PAGE_H
#define PG_TRE_SURF_PAGE_H

#include "postgres.h"
#include "storage/block.h"
#include "utils/rel.h"

#include "pg_tre/surf.h"

/* Write a serialized SuRF image across a SURF page chain; returns the
 * first block, or InvalidBlockNumber if len == 0. */
extern BlockNumber pg_tre_surf_write_image(Relation index,
										   const uint8 *image, Size len);

/* Read + deserialize the SuRF from the chain at `root` (NULL if none). */
extern PgTreSurf *pg_tre_surf_read(Relation index, BlockNumber root);

#endif /* PG_TRE_SURF_PAGE_H */
