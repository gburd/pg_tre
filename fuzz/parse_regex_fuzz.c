/*
 * fuzz/parse_regex_fuzz.c - libFuzzer harness for the pg_tre regex
 * parser, trigram extraction, and Navarro tiling.
 *
 * Build:
 *   cd fuzz && make -f Makefile.fuzz
 *
 * Run:
 *   ./pg_tre_fuzz corpus -max_total_time=900 -rss_limit_mb=2048 \
 *                 -detect_leaks=0
 *
 * Note: -detect_leaks=0 is recommended for long campaigns because the
 * thin backend shim in pg_backend_stub.c does not free per-iteration
 * MemoryContext allocations -- libpgcommon's frontend palloc has no
 * per-context tracking.  ASan still red-zones every chunk and catches
 * UAF/OOB on individual allocations.
 */

#include "postgres.h"

#include "lib/stringinfo.h"
#include "utils/memutils.h"

#include "pg_tre/regex_ast.h"
#include "pg_tre/tiling.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Defined in pg_backend_stub.c; set by the harness around each iteration. */
extern jmp_buf *pg_fuzz_error_jmp;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	TreParseCtx ctx;
	MemoryContext fuzz_context = NULL;
	MemoryContext old_context = NULL;
	jmp_buf error_buf;
	bool parse_ok = false;

	if (size == 0 || size > 4096)
		return 0;

	pg_fuzz_error_jmp = &error_buf;
	if (setjmp(error_buf) != 0)
	{
		/* ereport(ERROR) was called; clean up and return. */
		if (fuzz_context != NULL)
		{
			if (old_context != NULL)
				MemoryContextSwitchTo(old_context);
			MemoryContextDelete(fuzz_context);
		}
		pg_fuzz_error_jmp = NULL;
		return 0;
	}

	fuzz_context = AllocSetContextCreate(NULL,
										 "FuzzContext",
										 ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(fuzz_context);

	memset(&ctx, 0, sizeof(ctx));
	ctx.mcxt = fuzz_context;
	ctx.input = (const char *) data;
	ctx.input_len = (int) size;

	parse_ok = tre_parse_regex(&ctx, (const char *) data, (int) size);

	if (parse_ok && ctx.root != NULL)
	{
		for (int32 k = 0; k <= 2; k++)
		{
			TrigramQuery query;

			memset(&query, 0, sizeof(query));
			if (regex_extract_query(&ctx, k, &query) && k > 0 && query.n > 0)
			{
				TrigramQuery tiled;

				memset(&tiled, 0, sizeof(tiled));
				(void) pg_tre_tile_query(ctx.root, k, &tiled, fuzz_context);
			}
		}
	}

	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(fuzz_context);
	pg_fuzz_error_jmp = NULL;
	return 0;
}
