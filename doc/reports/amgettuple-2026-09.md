# pg_tre 4.0.1 — reply to the 3.2.5 `amgettuple` report

## Status: cannot reproduce, and I have run out of hypotheses I can test alone

You retracted the casing framing and re-aimed at scan path, which was the
right call and narrowed things usefully. I still cannot reproduce it, and I
want to be precise about what that does and does not mean.

### What I tested, all with the plain Index Scan forced

`SET enable_seqscan=off; SET enable_bitmapscan=off; SET enable_indexscan=on`,
confirmed via `EXPLAIN` to be `Index Scan using ...`:

| axis varied | result |
|---|---|
| casing: `^git`, `^GIT`, `^Git`, `^gIt` | all correct, index == seq |
| your table shape: `registry.packages`, `id bigserial/attr/name/version/system`, 7,352 rows, 3 rows named exactly `git` | correct |
| realistic nixpkgs-style names (dots, hyphens, `python3.11-`, `-bin`, `.dev`, `_lib`, `-unwrapped`) | correct |
| your other index present (`packages_name_prefix` on `lower(name) text_pattern_ops`) | correct |
| pending list **fully merged**, zero pending pages — your loose end | correct |
| long/hot posting chains (`git` trigrams in thousands of rows) | correct, 1,352 hits |
| index built, then upgraded in place across versions | correct |
| `ILIKE` variants | correct |

### Two corrections to the report's reasoning

**`Index Searches: 0` is not a fault signature.** It is the expected
instrumentation for this path: a case-insensitive predicate cannot be
trigram-accelerated, so extraction marks the query `always_true`, the scan
never descends the tree, and it streams every heap TID for the executor to
recheck. My *correct* runs show `Index Searches: 0` too. So the inference
that "the plain-tuple path rejects via the prefilter before searching" does
not follow.

**Your ask #1 is already satisfied.** The guard is not bitmap+KNN only.
`pg_tre_surf_prefilter_rejects()` has exactly two callers — `amgetbitmap`
and `amgettuple` — and the `always_true` check lives *inside* that shared
function, so both paths are covered by construction. 4.0.1 adds a
regression test that forces each scan path explicitly and requires both to
equal the sequential-scan ground truth (your ask #2); all 12 combinations
pass.

**On your ask #4:** the `Assert(!always_true)` would *not* have fired for
this. It only triggers if the prefilter is reached, and your `Index
Searches: 0` says the prefilter is not what returned empty. I appreciate the
credit but it would have been misplaced.

### What 4.0.1 adds so we can localise this from your side

The `amgettuple` path had **no** DEBUG instrumentation, which is the real
reason neither of us could localise your failure. It now emits, at `DEBUG1`:

```
pg_tre: amgettuple emitting N TIDs (always_true=1, candidates=C, orderby=0)
pg_tre: amgettuple always_true path streamed N heap TIDs (recheck will filter)
```

Please run this on the failing query:

```sql
SET client_min_messages = debug1;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM registry.packages WHERE name ~* '^GIT';
```

The output splits the problem cleanly:

- **`emitting 0 TIDs (always_true=1, ...)`** — the heap stream itself
  produced nothing. That points at the heap scan or `body_attno`
  resolution, not the index. Send me the line and I can go straight there.
- **`emitting N TIDs` with N > 0, but the query returns 0** — the index did
  its job and the executor's recheck dropped everything. That would make it
  an operator/collation issue outside pg_tre, and `SELECT name FROM
  registry.packages WHERE name ~* '^GIT'` under a seq scan vs the index
  would show which rows differ.
- **A `SuRF prefilter rejected scan` line** — then the guard *is* being
  bypassed somehow and I was wrong; that is immediately actionable.
- **No pg_tre DEBUG lines at all** — the running backend is not 4.0.1, i.e.
  a stale `.so`.

### One thing worth checking on your side regardless

Your `infra/k8s/pg18-image/flake.lock` still pins pg_tre `b8bb8bb` = **3.0.1**,
via a floating `?submodules=1` ref with no `ref=`. Nothing in the repo
references that image any more (`searchpg.yaml` runs `solnix-pg18-exts`,
correctly pinned), so I do **not** think it explains this report — I withdrew
that theory last time and it stays withdrawn. But it is a live footgun: any
future build of that image ships a version from before the SuRF tier
existed. Either delete the image or pin it to a tag.

### What I would need to go further

Failing the DEBUG output, a `pg_dump` of the 7,352-row table plus the exact
`CREATE INDEX` would let me load your data rather than synthesise it. Data
content is the one axis I genuinely cannot replicate — everything else I
have now varied.
