/*
 * fuzz/parse_regex_fuzz.c - libFuzzer harness for pg_tre regex parser.
 *
 * Tests tre_parse_regex(), regex_extract_query(), and pg_tre_tile_query()
 * with arbitrary byte sequences from libFuzzer.
 *
 * Build:
 *   cd fuzz && make -f Makefile.fuzz
 *
 * Run:
 *   ./pg_tre_fuzz corpus -max_total_time=900 -detect_leaks=1 -rss_limit_mb=2048
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

/* Stub declarations for Postgres types and memory management */
typedef struct MemoryContextData *MemoryContext;
typedef struct StringInfoData
{
    char   *data;
    int     len;
    int     maxlen;
} StringInfoData;
typedef StringInfoData *StringInfo;

/* Forward declarations from stubs */
extern MemoryContext AllocSetContextCreate(MemoryContext parent,
                                           const char *name,
                                           int minContextSize,
                                           int initBlockSize,
                                           int maxBlockSize);
extern void MemoryContextDelete(MemoryContext context);
extern void MemoryContextReset(MemoryContext context);
extern MemoryContext MemoryContextSwitchTo(MemoryContext context);

extern void *palloc(size_t size);
extern void *palloc0(size_t size);
extern void pfree(void *ptr);
extern void *repalloc(void *ptr, size_t size);

extern void initStringInfo(StringInfo str);
extern void appendStringInfoString(StringInfo str, const char *s);
extern void appendStringInfo(StringInfo str, const char *fmt, ...);
extern void appendStringInfoChar(StringInfo str, char c);

/* Error handling via setjmp/longjmp */
extern jmp_buf *pg_fuzz_error_jmp;
extern void ereport_noop(int level, const char *msg);

/* Parser and query API */
typedef struct TreParseCtx
{
    MemoryContext   mcxt;
    const char     *input;
    int             input_len;
    void           *root;       /* RegexAst* */
    int             syntax_error;
    char            errmsg[128];
    void           *tokenizer_state;
} TreParseCtx;

typedef struct TrigramQuery
{
    int             n;
    void           *conjuncts;
    int             min_match_len;
    int             global_max_cost;
    int             always_true;
    int             mode;
} TrigramQuery;

extern int tre_parse_regex(TreParseCtx *ctx, const char *pattern, int len);
extern int regex_extract_query(TreParseCtx *ctx, int max_cost, TrigramQuery *out);
extern int pg_tre_tile_query(const void *ast, int k, TrigramQuery *out, MemoryContext cxt);

/* libFuzzer entry point */
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    TreParseCtx ctx;
    MemoryContext fuzz_context = NULL;
    jmp_buf error_buf;
    volatile int parse_ok = 0;

    /* Skip inputs that are too large */
    if (size > 4096)
        return 0;

    /* Skip empty inputs */
    if (size == 0)
        return 0;

    /* Set up error handler */
    pg_fuzz_error_jmp = &error_buf;
    if (setjmp(error_buf) != 0)
    {
        /* ereport(ERROR) was called; clean up and return */
        if (fuzz_context)
            MemoryContextDelete(fuzz_context);
        pg_fuzz_error_jmp = NULL;
        return 0;
    }

    /* Create a per-iteration memory context */
    fuzz_context = AllocSetContextCreate(NULL,
                                         "FuzzContext",
                                         8 * 1024,   /* minContextSize */
                                         8 * 1024,   /* initBlockSize */
                                         8 * 1024 * 1024); /* maxBlockSize */
    MemoryContextSwitchTo(fuzz_context);

    /* Initialize parse context */
    memset(&ctx, 0, sizeof(TreParseCtx));
    ctx.mcxt = fuzz_context;
    ctx.input = (const char *) data;
    ctx.input_len = (int) size;

    /* Parse the regex */
    parse_ok = tre_parse_regex(&ctx, (const char *) data, (int) size);

    if (parse_ok && ctx.root != NULL)
    {
        /* Try extraction at k=0, k=1, k=2 */
        for (int k = 0; k <= 2; k++)
        {
            TrigramQuery query;
            memset(&query, 0, sizeof(TrigramQuery));

            /* Extract query */
            if (regex_extract_query(&ctx, k, &query))
            {
                /* If we got a valid query, try tiling for k>0 */
                if (k > 0 && query.n > 0)
                {
                    TrigramQuery tiled;
                    memset(&tiled, 0, sizeof(TrigramQuery));
                    (void) pg_tre_tile_query(ctx.root, k, &tiled, fuzz_context);
                }
            }
        }
    }

    /* Clean up */
    MemoryContextDelete(fuzz_context);
    pg_fuzz_error_jmp = NULL;

    return 0;
}
