# Phase A — pg_trgm capability parity

> **North star: pg_tre is a REGEX index with edit distance.**
> Phase A makes pg_tre a *superset* of pg_trgm's operator surface
> so users can run one extension, without changing what pg_tre
> fundamentally is.  No new index structure, no cracking — this
> phase is operator-class wiring plus one cheap-similarity
> primitive, reusing the trigram-lowering engine that already
> powers `%~~`.

**Status:** draft.  **Targets:** a 1.9.x line (no on-disk format
change).  **Depends on:** nothing (ship-first).

## A0. What we are reusing

`regex_extract_query()` (`src/query/extract.c` + `tiling.c`)
already lowers a parsed regex AST + an edit-distance budget `k`
into a `TrigramQuery`: a CNF/DNF of trigram conjuncts that the
scan path (`amgetbitmap`) resolves against posting trees.  Plain
literals, character classes, alternation, concatenation, and
Navarro `(k+1)`-tiling for `k>0` are all handled.

**Key consequence:** `LIKE`/`~`/`=` acceleration is *mapping new
operators onto this existing engine*, not new matching code.  A
`LIKE '%foo%'` is, for trigram-extraction purposes, the literal
substring `foo` at `k=0`.  A POSIX regex `~ 'fo+o'` is the same
AST path `%~~` already parses.

## A1. Operator-class members for LIKE / ILIKE / regex / equality

### Operators to add to `tre_text_ops`

| strategy | operator | left | right | lowering |
|---|---|---|---|---|
| 1 (exists) | `%~~` | text | tre_pattern | approximate regex (current) |
| 3 | `~~`  (LIKE) | text | text | LIKE pattern → literal-substring trigram AND, k=0 |
| 4 | `~~*` (ILIKE) | text | text | case-folded LIKE → trigram AND over lowercased trigrams, k=0 |
| 5 | `~`   (regex) | text | text | POSIX regex → AST → trigram extraction, k=0 |
| 6 | `~*`  (iregex) | text | text | case-folded regex, k=0 |
| 7 | `=`   (equality) | text | text | whole-string literal → trigram AND, k=0, recheck enforces exactness |

(`!~`, `!~*`, `NOT LIKE` are not directly indexable — negation
can't be answered by trigram presence; the planner correctly
falls back to seqscan, same as pg_trgm.)

Strategy numbers above 2 are free (1 = `%~~`, 2 = `<@>` ordering).
We keep pg_trgm's *semantics* but not its strategy numbers; the
planner binds by operator, not by number.

### Support functions

The opclass needs extract/consistent-style support so the AM can
turn an operator + RHS constant into a `TrigramQuery`:

- **`tre_extract_query(text pattern, int2 strategy) → internal`** —
  parse the RHS by strategy (LIKE-escape decode, regex parse, or
  literal) and produce the `TrigramQuery`.  This is a thin
  dispatcher in front of `regex_extract_query`.
- **`tre_consistent(...)** — for a lossy bitmap AM this is the
  recheck predicate; pg_tre is already recheck-based
  (`amgetbitmap` returns a lossy bitmap, executor rechecks via the
  operator's own function), so "consistent" is just the operator's
  match function (`textlike`, `texticlike`, `textregexeq`,
  `texteq`).  We reuse PostgreSQL's built-in match functions for
  recheck — we do **not** reimplement LIKE/regex matching.

### Scan-path changes (`amscan.c`)

`pg_tre_ambeginscan`/`amrescan` currently assume the RHS is a
`tre_pattern`.  Generalize: inspect `scan->keyData[i].sk_strategy`
and the RHS type; for the LIKE/regex/eq strategies, build the
`TrigramQuery` via the strategy-appropriate lowering, then the
existing `amgetbitmap` machinery is unchanged.  Recheck uses the
matching built-in (e.g. `textlike`) so results are exact.

### Why this is safe

- Every strategy is **lossy → recheck-authoritative**, exactly as
  pg_trgm GIN is.  The index narrows; the executor's recheck with
  the real operator guarantees correctness.  No semantic drift.
- The trigram engine is shared, so a LIKE and a `%~~` over the
  same literal touch the same posting trees — no duplication.

### Honest limit

For very short patterns (< 3 chars, no extractable trigram) the
extraction returns "always true" and the scan degenerates to a
full recheck — *identical* to pg_trgm's behavior on sub-trigram
patterns.  Documented, not hidden.

## A2. pg_trgm-compatible similarity: `%`, `<->`, `word_similarity`

These must mean what pg_trgm means — **trigram-set Jaccard** —
distinct from pg_tre's edit-distance `<@>`.  A user porting a
query that does `ORDER BY col <-> 'foo'` expects trigram distance,
not edit distance.

### Functions (new, Jaccard semantics over codepoint trigrams)

- `tre_trgm_similarity(text, text) → float4` — `|A∩B| / |A∪B|`
  over the two strings' trigram sets (pg_trgm `similarity()`).
- `tre_trgm_distance(text, text) → float4` — `1 - similarity`
  (pg_trgm `<->`).
- `tre_word_similarity(text, text)` / `tre_strict_word_similarity`
  — pg_trgm's word-boundary-aware variants.

### Operators

- `text % text → bool` — `similarity >= pg_tre.similarity_threshold`.
- `text <-> text → float4` — distance (orderable; integrate as a
  second ordering operator on the opclass so
  `ORDER BY col <-> 'foo'` can be index-assisted later, though A2
  only requires the functional/operator semantics).
- `text <% text`, `text <<-> text` — word-similarity operators.

### GUC

- `pg_tre.similarity_threshold` (PGC_USERSET, default 0.3, range
  0..1) — mirrors `pg_trgm.similarity_threshold`.  `%` honors it.

### Reuse

The codepoint-trigram extractor that the build path uses
(`extract_trigrams`) already produces the trigram set; the
Jaccard functions are a few set operations over two such sets.
No new index data is required — these are scalar functions like
pg_trgm's; index acceleration of `<->`/`%` is a later refinement,
not part of A2's correctness goal.

## A3. Finish `{~k}` planner integration (selectivity)

Today `tre_pattern_sel` returns a crude constant-ish selectivity,
so the planner's row estimate for `body %~~ tre_pattern('x', k)`
is a guess and plan choice is sometimes wrong (index when seqscan
wins, or vice-versa).

### Goal

A selectivity function keyed on the *structure* of the lowered
`TrigramQuery` and `k`:

- More required trigrams (longer literal runs) → lower
  selectivity (fewer matches) → favor index.
- Larger `k` → broader tiling → more disjuncts → higher
  selectivity → favor seqscan past a threshold.
- Use the index's own statistics where available: the meta page
  already tracks `n_tuples_indexed` and posting-tree counts; the
  per-trigram cardinality is knowable from posting-leaf headers.

### Approach

1. In `tre_pattern_sel`, lower the pattern to its `TrigramQuery`
   (same as the scan would), then estimate the AND-of-trigrams
   selectivity as the product of per-trigram selectivities
   (independence assumption, as pg_trgm does), each estimated from
   indexed cardinality / `n_tuples_indexed`.
2. Inflate for `k>0` by the tiling fan-out factor (number of
   disjuncts the budget produces).
3. Clamp to sane bounds; never return 0 or 1.

### Acceptance

`EXPLAIN` row estimates for representative `%~~` queries are
within ~2–3× of actual across selective and non-selective
patterns, so the planner stops choosing the index on
near-everything-matches patterns (the cause of the field-report
slow scans) and stops avoiding it on selective ones.

## Out of scope for Phase A

- Any change to on-disk format or the build path (that's Phase B).
- Index-side acceleration of `<->`/`%` ordering (functions +
  operators only in A2; KNN over Jaccard is a later item).
- BM25 / ranked full-text (permanently out of scope).

## Test plan

- `test/sql/like_accel.sql`: `LIKE`/`ILIKE`/`~`/`=` use the index
  (EXPLAIN shows index scan) AND return identical rows to a forced
  seqscan, including the sub-trigram fallback case.
- `test/sql/trgm_similarity.sql`: `tre_trgm_similarity` /
  `<->` / `%` numerically match `pg_trgm`'s `similarity`/`<->`/`%`
  on a shared corpus (install both extensions in the test, compare
  values).  `pg_tre.similarity_threshold` gates `%` correctly.
- `test/sql/sel.sql`: EXPLAIN row estimates within tolerance on a
  curated selective/non-selective pattern set.
- Existing 21 regression tests stay green; no format change.
