/*
 * fuzz/pg_backend_stub.c - thin backend shim for the fuzz harness.
 *
 * Real palloc/palloc0/pfree/repalloc, StringInfo, pg_utf_mblen_private,
 * pg_snprintf, etc. now come from libpgcommon.a + libpgport.a (linked
 * into the harness binary).  This file provides the small set of
 * symbols those libraries do not supply and that pg_tre's source
 * references via "postgres.h":
 *
 *   - CurrentMemoryContext, PG_exception_stack, error_context_stack
 *   - AllocSetContextCreateInternal, MemoryContextDelete, MemoryContextReset
 *     (no-op contexts; libpgcommon's palloc is malloc-backed and has no
 *     per-context tracking, so per-iteration cleanup happens by relying
 *     on libFuzzer to free the harness-side per-iteration MemoryContext
 *     pointer.  Allocations leak across iterations -- run with
 *     -detect_leaks=0 if running long campaigns.)
 *   - errstart/errstart_cold/errfinish/errcode/errmsg/errmsg_internal/
 *     elog_start/elog_finish/pg_re_throw/ExceptionalCondition  (longjmp
 *     out via pg_fuzz_error_jmp)
 *   - pg_tre_max_extraction_fanout (GUC backing variable from src/module.c)
 *
 * The full backend palloc with AllocSet semantics and MemoryChunk
 * headers is in the postgres binary, not in any installed library.
 * For UAF detection across context boundaries the harness would need
 * to be linked as a backend extension and run inside a real backend.
 */

#include "postgres.h"

#include "lib/stringinfo.h"
#include "utils/elog.h"
#include "utils/memutils.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

/* ----- Globals expected by backend headers ----- */

MemoryContext CurrentMemoryContext = NULL;
sigjmp_buf *PG_exception_stack = NULL;
ErrorContextCallback *error_context_stack = NULL;

/* GUC backing variable normally defined in src/module.c. */
int pg_tre_max_extraction_fanout = 4096;

/* Set by the libFuzzer harness around each iteration. */
jmp_buf *pg_fuzz_error_jmp = NULL;

/* ----- MemoryContext lifecycle (no-op shims) -----
 *
 * libpgcommon's palloc is malloc-backed and has no context awareness.
 * We therefore can't actually free everything in a context on
 * MemoryContextDelete; we just maintain CurrentMemoryContext as a
 * non-NULL sentinel so MemoryContextSwitchTo() works.
 *
 * MemoryContextData is the real backend struct (from nodes/memnodes.h).
 * We allocate a zeroed instance per call -- pg_tre's source only ever
 * compares the pointer for identity and dereferences via
 * MemoryContextSwitchTo, so the contents are irrelevant.
 */

static MemoryContextData fuzz_dummy_context;

MemoryContext
AllocSetContextCreateInternal(MemoryContext parent,
							  const char *name,
							  Size minContextSize,
							  Size initBlockSize,
							  Size maxBlockSize)
{
	(void) parent;
	(void) minContextSize;
	(void) initBlockSize;
	(void) maxBlockSize;

	MemoryContextData *ctx = calloc(1, sizeof(*ctx));

	if (ctx == NULL)
	{
		fprintf(stderr, "AllocSetContextCreateInternal: calloc failed\n");
		abort();
	}
	ctx->name = name;
	return ctx;
}

void
MemoryContextDelete(MemoryContext context)
{
	if (context == NULL || context == &fuzz_dummy_context)
		return;
	if (CurrentMemoryContext == context)
		CurrentMemoryContext = &fuzz_dummy_context;
	free(context);
}

void
MemoryContextReset(MemoryContext context)
{
	(void) context;
}

/* ----- ereport / elog plumbing -----
 *
 * Every error path longjmps back to the libFuzzer iteration.  All the
 * formatting helpers are sinks that return 0 and discard their args.
 */

static pg_noreturn void
fuzz_longjmp_or_abort(void)
{
	if (pg_fuzz_error_jmp != NULL)
		longjmp(*pg_fuzz_error_jmp, 1);
	abort();
}

bool
errstart(int elevel, const char *domain)
{
	(void) elevel;
	(void) domain;
	return true;
}

bool
errstart_cold(int elevel, const char *domain)
{
	return errstart(elevel, domain);
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
	(void) filename;
	(void) lineno;
	(void) funcname;
	fuzz_longjmp_or_abort();
}

int
errcode(int sqlerrcode)
{
	(void) sqlerrcode;
	return 0;
}

int
errmsg(const char *fmt,...)
{
	(void) fmt;
	return 0;
}

int
errmsg_internal(const char *fmt,...)
{
	(void) fmt;
	return 0;
}

int
errdetail(const char *fmt,...)
{
	(void) fmt;
	return 0;
}

int
errhint(const char *fmt,...)
{
	(void) fmt;
	return 0;
}

void
pg_re_throw(void)
{
	fuzz_longjmp_or_abort();
}

void
ExceptionalCondition(const char *conditionName,
					 const char *fileName,
					 int lineNumber)
{
	fprintf(stderr, "TRAP: failed assertion \"%s\" at %s:%d\n",
			conditionName, fileName, lineNumber);
	abort();
}
