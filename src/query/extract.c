/*
 * src/query/extract.c - regex AST to trigram Boolean formula.
 *
 * Phase 3: exact-regex extraction (k=0 case).
 * Implements Russ Cox's "Regular Expression Matching with a Trigram Index" (2012).
 *
 * Key idea: compute, for each AST node, the set of trigrams that MUST appear
 * in any string matching that sub-expression. Propagate these sets bottom-up
 * through CONCAT/ALT/REP, emit an AND-of-ORs query formula from the root.
 *
 * Properties computed per node:
 *  - exact_set: trigrams guaranteed to appear
 *  - prefix_set: possible 2-grams at the start
 *  - suffix_set: possible 2-grams at the end
 *  - match_set: trigrams extracted (may be approximate if node is complex)
 *  - emptyable: whether the node can match the empty string
 *
 * Phase 3 ASCII-only. UTF-8 support lands in Phase 3.5.
 */

#include "postgres.h"

#include "pg_tre/regex_ast.h"
#include "pg_tre/pg_tre.h"
#include "utils/memutils.h"

#include <string.h>

/*
 * Stub implementation for Phase 3.
 * Returns always_true = true (extraction defeated) until the real logic lands.
 * This allows the rest of the system to compile and be tested.
 */
bool
regex_extract_query(TreParseCtx *ctx, int32 max_cost, TrigramQuery *out)
{
	memset(out, 0, sizeof(TrigramQuery));
	out->always_true = true;
	out->n = 0;
	out->conjuncts = NULL;
	out->min_match_len = 0;
	out->global_max_cost = max_cost;
	return true;
}
