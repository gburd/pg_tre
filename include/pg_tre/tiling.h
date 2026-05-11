/*
 * include/pg_tre/tiling.h - Navarro tiling API for k>0 extraction.
 *
 * Phase 5: partition a pattern's trigram spine into k+1 tiles for
 * approximate matching via the pigeonhole principle.
 */

#ifndef PG_TRE_TILING_H
#define PG_TRE_TILING_H

#include "postgres.h"
#include "nodes/memnodes.h"

typedef struct RegexAst RegexAst;
typedef struct TrigramQuery TrigramQuery;

/*
 * Extract the trigram spine from an AST and tile it into k+1 groups.
 * Returns a TrigramQuery in DNF mode (OR across tiles, each tile is an
 * AND of its trigrams).  Returns false if extraction fails (sets
 * out->always_true = true).
 */
extern bool pg_tre_tile_query(const RegexAst *ast, int32 k,
                              TrigramQuery *out, MemoryContext cxt);

#endif /* PG_TRE_TILING_H */
