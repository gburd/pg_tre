/*
 * fuzz/memutils_stub.c - minimal PostgreSQL backend stubs for fuzzing.
 *
 * Provides just enough of palloc/MemoryContext/ereport to run the parser
 * and extraction code outside of a real PostgreSQL backend.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <assert.h>
#include <stddef.h>

/* ----- Type definitions ----- */

typedef struct MemoryContextData *MemoryContext;

typedef struct StringInfoData
{
    char   *data;
    int     len;
    int     maxlen;
} StringInfoData;

typedef StringInfoData *StringInfo;

/* ----- Memory tracking ----- */

typedef struct AllocChunk
{
    struct AllocChunk *next;
    MemoryContext context;
    size_t size;
    char data[1];  /* flexible array */
} AllocChunk;

typedef struct MemoryContextData
{
    const char *name;
    AllocChunk *chunks;
} MemoryContextData;

MemoryContext CurrentMemoryContext = NULL;

/* ----- Error handling ----- */

jmp_buf *pg_fuzz_error_jmp = NULL;

/* ----- GUC variables ----- */

int pg_tre_max_extraction_fanout = 1000;

/* ----- PG_TRY/PG_CATCH support ----- */

typedef struct ErrorContextCallback *ErrorContextCallback;

sigjmp_buf *PG_exception_stack = NULL;
ErrorContextCallback error_context_stack = NULL;

/* ----- Memory context API ----- */

MemoryContext
AllocSetContextCreate(MemoryContext parent,
                      const char *name,
                      int minContextSize,
                      int initBlockSize,
                      int maxBlockSize)
{
    MemoryContext ctx = (MemoryContext) malloc(sizeof(MemoryContextData));
    if (!ctx)
    {
        fprintf(stderr, "AllocSetContextCreate: malloc failed\n");
        abort();
    }
    ctx->name = name;
    ctx->chunks = NULL;
    return ctx;
}

void
MemoryContextDelete(MemoryContext context)
{
    AllocChunk *chunk, *next;

    if (!context)
        return;

    /* Free all chunks in this context */
    for (chunk = context->chunks; chunk != NULL; chunk = next)
    {
        next = chunk->next;
        free(chunk);
    }

    free(context);

    if (CurrentMemoryContext == context)
        CurrentMemoryContext = NULL;
}

void
MemoryContextReset(MemoryContext context)
{
    AllocChunk *chunk, *next;

    if (!context)
        return;

    /* Free all chunks */
    for (chunk = context->chunks; chunk != NULL; chunk = next)
    {
        next = chunk->next;
        free(chunk);
    }
    context->chunks = NULL;
}

MemoryContext
MemoryContextSwitchTo(MemoryContext context)
{
    MemoryContext old = CurrentMemoryContext;
    CurrentMemoryContext = context;
    return old;
}

/* ----- palloc family ----- */

void *
palloc(size_t size)
{
    AllocChunk *chunk;
    size_t total_size = sizeof(AllocChunk) + size;

    chunk = (AllocChunk *) malloc(total_size);
    if (!chunk)
    {
        fprintf(stderr, "palloc: malloc(%zu) failed\n", size);
        abort();
    }

    chunk->context = CurrentMemoryContext;
    chunk->size = size;
    chunk->next = NULL;

    if (CurrentMemoryContext)
    {
        chunk->next = CurrentMemoryContext->chunks;
        CurrentMemoryContext->chunks = chunk;
    }

    return chunk->data;
}

void *
palloc0(size_t size)
{
    void *ptr = palloc(size);
    memset(ptr, 0, size);
    return ptr;
}

void
pfree(void *ptr)
{
    AllocChunk *chunk;
    AllocChunk **prev_link;

    if (!ptr)
        return;

    /* Find the chunk header */
    chunk = (AllocChunk *) ((char *) ptr - offsetof(AllocChunk, data));

    /* Unlink from context if it has one */
    if (chunk->context)
    {
        prev_link = &chunk->context->chunks;
        while (*prev_link != NULL)
        {
            if (*prev_link == chunk)
            {
                *prev_link = chunk->next;
                break;
            }
            prev_link = &(*prev_link)->next;
        }
    }

    free(chunk);
}

void *
repalloc(void *ptr, size_t size)
{
    AllocChunk *old_chunk;
    void *new_ptr;

    if (!ptr)
        return palloc(size);

    old_chunk = (AllocChunk *) ((char *) ptr - offsetof(AllocChunk, data));

    new_ptr = palloc(size);
    memcpy(new_ptr, ptr, old_chunk->size < size ? old_chunk->size : size);
    pfree(ptr);

    return new_ptr;
}

/* ----- StringInfo family ----- */

void
initStringInfo(StringInfo str)
{
    str->data = (char *) palloc(1024);
    str->maxlen = 1024;
    str->len = 0;
    str->data[0] = '\0';
}

void
appendStringInfoString(StringInfo str, const char *s)
{
    int slen = strlen(s);
    int needed = str->len + slen + 1;

    if (needed > str->maxlen)
    {
        str->maxlen = needed * 2;
        str->data = (char *) repalloc(str->data, str->maxlen);
    }

    memcpy(str->data + str->len, s, slen + 1);
    str->len += slen;
}

void
appendStringInfoChar(StringInfo str, char c)
{
    if (str->len + 2 > str->maxlen)
    {
        str->maxlen *= 2;
        str->data = (char *) repalloc(str->data, str->maxlen);
    }

    str->data[str->len++] = c;
    str->data[str->len] = '\0';
}

void
appendStringInfo(StringInfo str, const char *fmt, ...)
{
    va_list args;
    int avail, nwritten;

    for (;;)
    {
        avail = str->maxlen - str->len;

        va_start(args, fmt);
        nwritten = vsnprintf(str->data + str->len, avail, fmt, args);
        va_end(args);

        if (nwritten < avail)
        {
            str->len += nwritten;
            break;
        }

        /* Need more space */
        str->maxlen = str->maxlen * 2 + nwritten;
        str->data = (char *) repalloc(str->data, str->maxlen);
    }
}

/* ----- Error reporting ----- */

void
ereport_noop(int level, const char *msg)
{
    /* Jump back to fuzzer if we have a jump buffer */
    if (pg_fuzz_error_jmp)
        longjmp(*pg_fuzz_error_jmp, 1);

    /* Otherwise just return */
}

int
errstart(int elevel, const char *domain)
{
    /* Always return true so error messages get evaluated */
    return 1;
}

int
errfinish(int dummy)
{
    if (pg_fuzz_error_jmp)
        longjmp(*pg_fuzz_error_jmp, 1);
    return 0;
}

int
errcode(int sqlerrcode)
{
    return 0;
}

int
errmsg(const char *fmt, ...)
{
    return 0;
}

void
elog_start(const char *filename, int lineno, const char *funcname)
{
}

void
elog_finish(int elevel, const char *fmt, ...)
{
    if (pg_fuzz_error_jmp)
        longjmp(*pg_fuzz_error_jmp, 1);
}

int
errstart_cold(int elevel, const char *domain)
{
    return errstart(elevel, domain);
}

void
pg_re_throw(void)
{
    if (pg_fuzz_error_jmp)
        longjmp(*pg_fuzz_error_jmp, 1);
}

int
errmsg_internal(const char *fmt, ...)
{
    return 0;
}

int
pg_snprintf(char *str, size_t count, const char *fmt, ...)
{
    va_list args;
    int result;

    va_start(args, fmt);
    result = vsnprintf(str, count, fmt, args);
    va_end(args);

    return result;
}

int
pg_fprintf(void *stream, const char *fmt, ...)
{
    /* Ignore debug output from the Lime parser */
    return 0;
}

void
ExceptionalCondition(const char *conditionName,
                     const char *fileName,
                     int lineNumber)
{
    fprintf(stderr, "TRAP: %s(\"%s\", File: \"%s\", Line: %d)\n",
            "ExceptionalCondition", conditionName, fileName, lineNumber);
    abort();
}

/* ----- UTF-8 support ----- */

/*
 * pg_utf_mblen_private - return length of a UTF-8 multibyte sequence
 *
 * Copied from PostgreSQL src/common/wchar.c
 * Note: PG18 renamed this from pg_utf_mblen to pg_utf_mblen_private
 */
int
pg_utf_mblen_private(const unsigned char *s)
{
    int len;

    if ((*s & 0x80) == 0)
        len = 1;
    else if ((*s & 0xe0) == 0xc0)
        len = 2;
    else if ((*s & 0xf0) == 0xe0)
        len = 3;
    else if ((*s & 0xf8) == 0xf0)
        len = 4;
    else
        len = 1;

    return len;
}
