# Bug report: out-of-bounds read in `src/query/uleven.c`, still present at 4.0.2

**To:** pg_tre maintainers
**From:** pg_weave (imports pg_tre's fuzzy/regex query-compilation subsystem, commit
`e03d6a833170c9b58b709a8845f30d52685dce69`, MIT, same author as pg_weave)
**Affects:** `pg_tre` `src/query/uleven.c`, functions `pg_tre_uleven_expand()` and
`pg_tre_uleven_expand_cp()`, confirmed present in your working tree at version 4.0.2
(checked directly against `/home/gburd/ws/pg_tre`, not against a copy)
**Date:** 2026-09-21

## One thing withdrawn before the report, because it turned out not to apply

Our own project documentation (`doc/PHASES.md` task Z5, and the header comment in our
`include/weave/uleven.h`) describes a fix to "a `nextkey` scratch buffer sized
`WEAVE_LEV_MAXQ + 2` that an edit budget larger than the query length could overrun."
We went looking for the corresponding buffer in your source to cite it here, and it is
**not there**: `grep -rn "MAXQ\|nextkey" include/pg_tre/uleven.h src/` in your tree
returns nothing. That buffer and the `WEAVE_LEV_MAXQ` constant are in our own
`include/weave/lev.h` and `src/am/amscan.c` — code we inherited from pg_fts at our fork
point (pg_fts 1.5.8), not code we imported from you. **That specific claim is
WITHDRAWN.** It would have been a report about our own ancestry, misfiled against you.

## What we found instead, searching your actual `uleven.c`

Your `src/query/uleven.c` and `include/pg_tre/uleven.h` contain a different mechanism
than the one described above: a byte-alphabet neighbourhood expansion of a fixed
three-byte trigram (`pg_tre_uleven_expand`, `pg_tre_uleven_expand_cp`), used by
`src/query/tiling.c` to widen a regex trigram spine. We imported this file verbatim at
the commit named above, and while re-working the surrounding code (our task Z5) we
found and fixed an out-of-bounds/uninitialized read in our copy of it. **Your copy
still has the exact code we changed.** This is the report we can actually substantiate
against your current source, by file and line.

### The mechanism, in your file as it stands today

`pg_tre_uleven_expand()`, `k == 2` branch, `src/query/uleven.c:223-255`:

```c
int n1 = uleven_expand_k1(tri, temp, 16384);          /* line 228 */
...
n = 0;
for (i = 0; i < n1; i++)                              /* line 233 */
{
    batch = uleven_expand_k1(temp[i], &temp[n1], 16384 - n1);   /* line 236 */
    ...
    int j;
    for (j = 0; j < batch && (n + j) < 16384; j++)     /* line 242 */
    {
        if (n + j >= max_out)
            return -1;
        out[n + j][0] = temp[n1 + j][0];
        ...
    }
    n += batch;                                        /* line 250 */
}

return dedupe_trigrams(out, n);                         /* line 254 */
```

`out` is a caller-supplied buffer whose capacity is `max_out` (see
`pg_tre_uleven_expand_cp()` at `src/query/uleven.c:325-326`, which does
`btmp = palloc(sizeof(uint8[3]) * max_out); n = pg_tre_uleven_expand(btri, k, btmp,
max_out);` — the allocation size and the `max_out` argument are the same number).

The copy loop at line 242 stops copying once `n + j` reaches **16384**, independent of
`max_out`. But `n += batch` at line 250 runs on every iteration of the outer loop
regardless of how many entries the inner loop actually copied. Once `n` reaches 16384,
the inner loop's own guard `(n + j) < 16384` is false for `j = 0`, so the inner loop body
— including the `if (n + j >= max_out) return -1` overflow check — **never executes
again**, while the outer loop keeps running and `n` keeps growing on every remaining
`i`. `dedupe_trigrams(out, n)` at line 254 is then called with `n` that can exceed
`max_out`, reading `out[0 .. n)` — a buffer allocated for only `max_out` entries. That
is an out-of-bounds read for any `i` such that `out[i]` was never written and lies past
the allocation.

**This requires `max_out > 16384` to trigger** — with `max_out <= 16384`, the
`if (n + j >= max_out) return -1` check inside the inner loop is guaranteed to fire
before `n` reaches 16384, so the function returns `-1` safely instead of overrunning.

### Reachability in your own tree today

Your only two callers, both in `src/query/tiling.c:195` and `:227`, pass a fixed
`int32 expanded[4096][3]` buffer and `max_out = 4096` — well under the 16384 threshold,
so this is **not reachable through your own current call sites**. We checked this
specifically because it is the same shape of finding we made in our own tree (our
`nextkey` buffer, withdrawn above, was likewise unreached by any test in our tree). It
is, however, reachable by any caller of the exported functions
`pg_tre_uleven_expand()` / `pg_tre_uleven_expand_cp()` — both declared `extern` in
`include/pg_tre/uleven.h:22` and `:33`, i.e. public API — with `max_out` above 16384.
We have not audited every consumer of your public API outside your own tree, so we
cannot state whether any current caller does this; we can state that the function's
contract does not prevent it and its own doc comment ("Returns -1 if the expansion
would exceed max_out (overflow)") promises a safe `-1` in exactly the case where it does
not deliver one.

### What we did on our side

We imported this file unmodified at first (our commit history: the trigram-expansion
file entered our tree as `26a70da`), then found this while re-working the surrounding
Z5 code and fixed it in our own copy (`src/query/uleven.c`, our commit `376d18c`,
"uleven: correct a false provenance claim and close an unreachable overrun"). Our fix
adds an explicit upfront bound check before the copy loop:

```c
if (n > 16384 - batch || (max_out >= batch && n > max_out - batch))
    return -1;      /* would overrun `out` or the dedupe input */
```

which returns `-1` in exactly the case the original code was supposed to but did not,
before doing any copying for that batch. We are not asserting this is the only correct
fix or the one you would want — only that it is what we shipped and verified against
our own copy of the file.

We also corrected the file's header comment while we were in there: it had called this
"Mihov-Schulz universal Levenshtein automaton", which it is not — there is no automaton
in it, it is a brute-force enumeration of a fixed-size neighbourhood. That claim isn't a
bug, but if it is your comment too (we imported it from your file, so it very likely is)
it may be worth correcting on your side independent of the overrun.

## What was NOT measured or checked

- We did not fuzz or exploit this to demonstrate an observable crash or memory
  disclosure; we traced the control flow by reading the code and confirmed the
  boundary conditions above match a real overrun of an allocated buffer, but we did
  not run it under ASan or a debugger against your build.
- We did not check whether any downstream consumer of your public API (outside your
  own `tiling.c`) calls these functions with `max_out > 16384`.
- We did not check whether this is fixed in an unreleased branch past 4.0.2 — we
  checked the working tree at `/home/gburd/ws/pg_tre`, whose `META.json` reports
  version 4.0.2 and whose most recent commit touching `src/query/uleven.c` predates
  4.0.2 in your own `git log`.
- We did not check `dedupe_trigrams_cp()` (the codepoint-alphabet twin) for an
  analogous defect independent of this one — it is a simpler O(n^2) dedup with no
  16384-sized scratch buffer of its own, so the same failure shape does not obviously
  apply, but we did not trace every call path into it as carefully as the byte-alphabet
  version above.

## What we would need from you

Nothing required. If useful: a confirmation of whether you consider the public API's
missing bound the right thing to fix (versus, say, documenting that `max_out` must not
exceed 16384 for `k == 2`), and whether a patch matching our fix — or your own, adapted
to your current code — is something you'd want submitted. We are a fork/importer of
your MIT-licensed code under a PostgreSQL license, so the flow of a patch back to you is
one-directional by licence, not by preference, and we are happy to send one.
