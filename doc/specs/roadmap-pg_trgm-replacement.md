# pg_tre roadmap: standalone replacement for pg_trgm

> **North star: pg_tre is a REGEX index with edit distance.**
> Everything in this roadmap exists to let users adopt pg_tre
> *instead of* pg_trgm without giving up substring/`LIKE`/regex
> acceleration or cheap similarity — never to turn pg_tre into a
> general-purpose or full-text engine.  BM25 / TF-IDF ranked
> full-text is explicitly **out of scope**; the documented answer
> for ranked full-text remains "pair pg_tre with `tsvector`."

This document is the umbrella; each phase has its own detailed
spec under `doc/specs/`.  Phases are sequenced so that the
bounded, low-risk, immediately-useful work ships first and the
high-novelty research work is gated behind it.

## Why "replace pg_trgm" is two goals, not one

pg_trgm does three jobs:

1. **Operator acceleration** — the planner rewrites
   `col LIKE '%foo%'`, `col ~ 'fo+o'`, `col = 'foo'` into trigram
   set-membership scans against a GIN/GiST index.  This is a
   *planner-integration contract*, not an algorithm.
2. **Cheap similarity** — `%`, `<->`, `word_similarity`:
   trigram-set Jaccard, fixed and stateless.
3. (pg_tre's unique third job) **edit-distance / approximate-regex
   matching** with per-subexpression `{~k}` budgets — which
   pg_trgm cannot do at all.

To retire pg_trgm we must win **(1)** and **(2)** — the cases
where pg_trgm is fast and small — while keeping **(3)**.  That
splits into:

- **Goal A — capability-replace:** support every operator pg_trgm
  supports so a user can `DROP EXTENSION pg_trgm;` and keep one
  index.  *Accepts* that pg_tre may be slower/larger than pg_trgm
  GIN on the exact-substring case.  Bounded, shippable.
- **Goal B — performance-replace:** also bound build memory and
  improve query cost at scale via an adaptive, incrementally-built
  ("cracked") index, so the single-extension story is also viable
  on large corpora.  Higher novelty, gated, staged.

## Current state (as of 1.8.2)

What already works:

- `%~~` (approximate regex match) is an indexable operator
  (strategy 1) with cost estimation (`tre_pattern_sel`).
- `<@>` (edit distance) is an ordering operator (strategy 2,
  `amcanorderbyop = true`) — `WHERE body %~~ p ORDER BY body <@> p
  LIMIT n` plans as an index scan with no executor Sort.
- `tre_distance` / `tre_similarity` functions exist (edit-distance
  semantics).
- Build memory is bounded by `maintenance_work_mem` (tuplesort,
  1.8.0).  `CREATE INDEX CONCURRENTLY` / `REINDEX CONCURRENTLY`
  work (verified 1.7.0).

What is missing for "no longer need pg_trgm":

- **No `LIKE`/`ILIKE`/`~`/`!~`/`=` planner acceleration.**  These
  operators do not use a pg_tre index today.  (Goal A.)
- **No pg_trgm-compatible similarity** (`%`, `<->`,
  `word_similarity` with trigram-set Jaccard semantics).  pg_tre's
  `<@>` is *edit distance*, which is a different number with
  different meaning.  (Goal A.)
- **`{~k}` selectivity estimation is crude** — the index is
  pickable but the planner's row estimates for fuzzy predicates
  are guesses, so plan choice is sometimes wrong.  (Goal A.)
- **Build is not incremental / workload-adaptive** — a full index
  is built up-front; there is no lazy per-trigram materialization,
  no aging, no self-collapsing structure.  (Goal B.)

## Phases

### Phase A — capability parity (ship first; no LSM, no cracking)
Spec: [`doc/specs/phaseA-pg_trgm-parity.md`](specs/phaseA-pg_trgm-parity.md)

1. Operator-class members + support functions so `LIKE`/`ILIKE`/
   POSIX-regex (`~`/`~*`/`!~`/`!~*`) and `=` lower to the existing
   trigram engine and use a pg_tre index.
2. A second, *cheap* similarity primitive: `%`, `<->`,
   `word_similarity`, `strict_word_similarity` with
   trigram-set-Jaccard semantics matching pg_trgm, plus the
   `pg_tre.similarity_threshold` GUC analog.
3. Finish `{~k}` planner integration: real selectivity estimation
   keyed on pattern structure + k, so the planner costs fuzzy
   predicates accurately.

Phase A alone achieves capability-replace: a user can install only
pg_tre and keep `LIKE`, `~`, `%`, `<->`, plus gain edit-distance
regex.

### Phase B1 — adaptive LSM substrate (incremental build, 1-D)
Spec: [`doc/specs/phaseB-adaptive-lsm-cracking.md`](specs/phaseB-adaptive-lsm-cracking.md)

Re-cast the build/ingest path on the adaptive-LSM model from
`~/ws/aether/src/lsm` (Hanoi-tower leveling, newest-wins +
tombstone merge, overwrite-on-merge, and crucially the
**adaptive collapse back to a single clean run** under low write
pressure).  The existing `fastupdate` pending list becomes L0.
**No visibility dimension yet** — normal `amgetbitmap` +
executor recheck; correctness-conservative.  Delivers
incremental, low-RAM, workload-adaptive index growth.

### Phase B2 — visibility-aware 2-D LSM (the research bet)
Spec: [`doc/specs/phaseB-adaptive-lsm-cracking.md`](specs/phaseB-adaptive-lsm-cracking.md) (§ B2)

Add a second LSM dimension keyed on visibility (64-bit
full-transaction-id / commit-LSN, wraparound-safe) so runs are
bounded by both *which trigrams they own* and *which snapshot
window they are valid for*.  VACUUM drops visibility slices below
the freeze horizon.  This is what lets pg_tre memoize candidate
result-sets and prune whole runs by snapshot horizon — staying in
harmony with the executor (snapshot-scoped run pruning), without
*correctness* depending on it.  Gated on a written
wraparound-correctness invariant and explicit standby + crash
answers.

## Explicitly out of scope

- **BM25 / TF-IDF ranked full-text.**  Different index, different
  model (tokenized, stemmed, frequency-ranked) — the opposite of
  pg_tre's character-trigram, stemming-free, edit-distance design.
  The Phase A Jaccard `<->` similarity covers "rank fuzzy matches"
  cheaply; ranked full-text stays a `tsvector` pairing.
- **Becoming a general text-search engine.**  Every feature here
  is justified by the north star or it does not ship.
