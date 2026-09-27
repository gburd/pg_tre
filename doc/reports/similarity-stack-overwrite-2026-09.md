# Stack overwrite in `tre_trgm_similarity` and the word-similarity family (2026-09)

**Fixed in 4.2.0.** Present in every release from 1.9.0 through 4.1.0.

## Report

During downstream qualification on PostgreSQL 18.6 (Debian `postgres:18.6`,
x86_64), with both pg_tre 4.0.2 and 4.1.0:

```sql
SELECT tre_trgm_similarity('foo','foobar');
-- server closed the connection unexpectedly
```

The reporter's original run recorded `stack smashing detected` / SIGABRT and
server recovery. Plain ASCII input; no table, index or concurrency needed. The
reporter proposed a root-cause lead and was explicit that it was unvalidated.
The lead was correct.

## Root cause

`src/query/trgm_similarity.c`, in both `trgm_set()` and `pos_trgm()`:

```c
pg_wchar wc;
pg_mb2wchar_with_len(p, &wc, clen);
```

`pg_mb2wchar_with_len()` always writes a terminating zero after the decoded
character (`*to = 0` at the end of `pg_utf2wchar_with_len` in
`src/common/wchar.c`). That puts two `pg_wchar`s into a one-`pg_wchar`
variable, on every character of every call. The extra 4-byte store goes into
whatever the compiler placed next to `wc` in the frame:

- in most builds, a dead slot, so nothing visible happens;
- in the reporter's build, the stack-protector canary, so the backend aborts.

That dependence on frame layout explains why the bug lasted eight releases.
The regression suite has always called `tre_trgm_similarity('foo','foobar')`
and got `0.375`.

## Fix

```c
pg_wchar wbuf[MAX_MULTIBYTE_CHAR_LEN + 1];
pg_mb2wchar_with_len(p, wbuf, clen);
wc = wbuf[0];
```

This leaves room for the terminator and for the worst case of `clen` wchars.
The conversion emits at most one wchar per input byte, and `clen` is clamped
to 1 when a sequence would run past the end. The tree has no other
`pg_*2wchar*` call.

## Evidence

| Build | Statement | Result |
|---|---|---|
| unfixed, **ASan-instrumented server** | `tre_trgm_similarity('foo','foobar')` | backend lost; ASan `stack-buffer-overflow`, `WRITE of size 4` in `pg_utf2wchar_with_len` ← `pg_mb2wchar_with_len` ← `trgm_set` (`:94`), `'wc' ... overflows this variable` |
| unfixed, ASan server | `tre_word_similarity('foo','foo bar')` | backend lost; same report from the **second** site, `pos_trgm` (`:343`) |
| fixed, ASan server | both, plus `test/sql/similarity_multibyte.sql` | `0.375`, `1`; ASan silent |
| unfixed, stock `-O2` (with or without `-fstack-protector-all`) | reporter's statement | returns `0.375`: **does not reproduce** |
| unfixed, ASan applied to the extension only | reporter's statement | **ASan silent**: the offending store is in the uninstrumented server |

The last two rows are the reason the bug survived: a normal build gives no
sign of it, and even sanitizing pg_tre alone misses it. It only shows up when
PostgreSQL itself is instrumented.

## Affected functions

All eight share the two helpers:

`tre_trgm_similarity`, `tre_trgm_distance`, `tre_trgm_sim_op`,
`tre_word_similarity`, `tre_strict_word_similarity`, `tre_word_sim_op`,
`tre_word_dist_op`, `tre_strict_word_sim_op`, `tre_strict_word_dist_op`.

**Not affected:** the index access method, regex / `LIKE` / `ILIKE` matching,
and Levenshtein. The reporter's application uses only those, so their deployed
pin is not exposed.

## Regression coverage

`test/sql/similarity_multibyte.sql` covers the reporter's statement,
empty/identical/disjoint controls, 1-, 2-, 3- and 4-byte UTF-8 characters
through every entry point on both helpers, and a 10,000-character input.

In a normal build its counts **cannot** catch this overwrite, because the
overwrite is silent there. The file only detects the bug when run against an
ASan-instrumented server, which is how it was validated.

## Methodology note

If a report says a memory-safety bug reproduces elsewhere, "it passes here" is
not evidence that it doesn't exist. Here the stack protector did not fire at
all on the qualification rig. The case was closed by instrumenting the code
that does the write.
