# Phase B1 — design: on-disk run/level catalog (format v7)

> **North star: pg_tre is a REGEX index with edit distance.** B1
> changes *how* index data is organized and grown, not what it
> matches. The executor recheck contract is untouched.

**Status:** in progress. **Target:** 2.0 line (on-disk format
change v6 → v7). **Depends on:** Phase A (shipped, v1.12.0).

This document specifies the *first, smallest, reversible*
increment of B1: the run/level catalog data structure. It does
**not** yet change merge behavior — a freshly built or upgraded
index has exactly **one run**, so its scan path and cost are
identical to today's (the spec's "worst case = today" guarantee,
realized as the *default* case until later increments add
multi-run flushing).

## Why catalog-first

B1 is large (run flushing, Hanoi leveling, multi-run scan-merge,
adaptive collapse, lazy materialization). Attempting it all at
once is unreviewable and unrevertable. The catalog is the
substrate every later piece reads and writes, and it can land
behind a single-run invariant that is observably a no-op:

- A new v7 index has one run (run id 0) at level 1 whose roots
  are exactly today's `root_upper` / `root_range`.
- `amgetbitmap` over a one-run catalog is byte-for-byte the
  current scan.
- Tests assert from-scratch v7 builds produce identical results
  to v6.

Later increments (separate commits, same 2.0 line) flip the
behavior on: pending-flush-to-run, Hanoi merge, multi-run scan,
collapse.

## The unit: a *run*

A run is "a small pg_tre index over a slice of ingested rows" —
it reuses the existing posting-tree + upper-tree + range-tier
layout. A run is fully described by:

```
typedef struct PgTreRun
{
    uint64      run_id;          /* monotonic, never reused, 64-bit */
    uint32      level;           /* Hanoi level (1-based; 0 = nursery) */
    uint32      flags;           /* PG_TRE_RUN_* */
    BlockNumber root_upper;      /* this run's upper-tree root */
    BlockNumber root_range;      /* this run's range-tier root */
    uint64      n_tuples;        /* rows contributed to this run */
    uint64      n_trigrams;      /* distinct trigrams in this run */
    uint64      min_trigram_hash;/* run-skip range filter: lo */
    uint64      max_trigram_hash;/* run-skip range filter: hi */
} PgTreRun;                      /* 64 bytes, 8-aligned */
```

`run_id` is **monotonic and 64-bit** — it never wraps and never
reuses a freed id (matching aether's `RunId`). It is *not* a
transaction id; B1 has no visibility axis (that is B2). The
keyspace range filter (`min/max_trigram_hash`) is the B1
analogue of aether's `Surf` per-run skip filter: a scan whose
queried trigram hash falls outside `[min,max]` skips the run.

## Catalog storage

The catalog is small (one 64-byte entry per run; a healthy index
has O(log n) runs, collapsing toward 1). It lives in a dedicated
**catalog page chain** rooted from the meta page, not inline in
the meta page (the meta page's `reserved[27]` is only 108 bytes —
enough for the *header*, not an unbounded run list).

Meta-page additions (consume `reserved[]`, no struct-size change):

```
uint64      next_run_id;        /* monotonic allocator; 0 means "pre-v7" */
BlockNumber run_catalog_head;   /* first catalog page; Invalid = one
                                 * implicit run from root_upper/range */
uint32      n_runs;             /* total live runs */
uint32      max_levels;         /* Hanoi cap (default 7) */
```

When `run_catalog_head == InvalidBlockNumber` (the default for a
freshly upgraded v6→v7 index and for a from-scratch v7 build that
has not yet flushed a second run), the index has **one implicit
run** whose roots are the meta page's `root_upper`/`root_range`.
This is the no-op case: zero catalog pages, zero behavior change.

A catalog page is an array of `PgTreRun` after the standard
`PageTreOpaque`, chained via `right_link` when more runs exist
than fit on one page (≈126 runs/page — far more than any sane
index needs, so in practice one page).

## On-disk format version

```
PG_TRE_FORMAT_VERSION_LATEST = 7   (was 6)
PG_TRE_FORMAT_VERSION_MIN    = 6   (v6 still readable; upgraded lazily)
```

A v6 index is read as "one implicit run" with no catalog page —
identical to the v7 default case — so **v6 indexes work unchanged
under v7 code with no REINDEX**. (Contrast sparsemap 4.0.0's hard
break; the run catalog is purely additive.) `pg_tre_upgrade_index`
stamps v7 and initializes `next_run_id`/`n_runs`/`max_levels`
without rewriting posting data.

## Scan: multi-run union (single-run = today)

`amgetbitmap` iterates live runs (newest `run_id` first), and for
each queried trigram:

1. Skip the run if the trigram hash is outside the run's
   `[min,max_trigram_hash]` (the Surf analogue).
2. Otherwise resolve the trigram against the run's upper/range
   roots exactly as today.
3. Union the candidate TID sparsemaps across runs with
   **newest-run-wins on (trigram, TID)**; a tombstoned TID in a
   newer run subtracts it from older runs (aether `merge_range`).

With one run, steps reduce to today's single resolution; the
union/tombstone machinery is identity. B1's later increment turns
on the multi-run path; this increment ships the catalog + the
iteration scaffold that degenerates to the current scan.

Recheck is still always lossy/true: the executor re-applies the
real operator per row. No MVCC contract is touched.

## Crash recovery / replication / standby

- Catalog pages are WAL'd like every other pg_tre page; a standby
  replays them. No read-side writes in B1 (collapse/crack-on-read
  is a later increment and will be a standby no-op, per B2's
  constraint 3, brought forward).
- `next_run_id` is advanced in the same WAL'd meta update that
  introduces a run, so a replayed/forked cluster never reuses an
  id.
- Worst case after crash = a valid multi-run index that a later
  VACUUM/compaction collapses; never assumed durable-and-warm
  beyond what WAL guarantees.

## This increment's deliverables (one commit)

1. `PgTreRun` struct + catalog page kind + reader/writer.
2. Meta-page fields (`next_run_id`, `run_catalog_head`, `n_runs`,
   `max_levels`) carved from `reserved[]`.
3. Format bump v6 → v7; v6 read as one implicit run (no REINDEX).
4. `pg_tre_upgrade_index` stamps v7 + catalog header.
5. A run-iteration API (`pg_tre_run_catalog_open` /
   `_next` / `_close`) that yields the one implicit run for v6 and
   for default v7, used by a *refactored but behavior-identical*
   `amgetbitmap`.
6. Tests: v7 from-scratch == v6 results; v6→v7 in-place upgrade,
   no REINDEX, identical results; catalog round-trips.

Explicitly **not** in this increment: pending-flush-to-run, Hanoi
merge, real multi-run state, collapse, lazy materialization. Those
are subsequent commits on the 2.0 line, each behind tests.

## B1.2 status (multi-run scan landed; catalog writer deferred)

Delivered:

- **Multi-run scan path.** `resolve_conjunct_with_overlay` iterates
  the run catalog and unions each query trigram's postings across
  all runs, applying the per-run `[min,max]` trigram-hash run-skip
  filter (the aether `Surf` analogue).  For the single implicit
  run this is byte-identical to the pre-v7 scan.
- **`pg_tre_upper_lookup_root`** resolves a trigram against an
  arbitrary run root (vs. only the index's `root_upper`).
- **Tier-3 / positional refinements gated to single-run.** Those
  lossy optimizations resolve against the single index root, so
  with >1 run they are skipped; the executor recheck stays
  authoritative, so results remain correct, just less pre-filtered.

Deferred (NOT shipped) -- the catalog **writer**:

- A WAL-logged `pg_tre_run_catalog_append` was prototyped but
  **removed before shipping** because its crash-recovery replay was
  not durable under repeated appends to the same catalog page: a
  single post-checkpoint append replayed correctly, but several
  rapid appends to one catalog page lost the meta-page update on
  `kill -9` recovery.  Two bugs were found and fixed along the way
  (a `REGBUF_WILL_INIT` FPI PANICs the generic `pg_tre_redo_fpi`;
  page edits must be inside the critical section), but a residual
  replay gap for the multi-FPI-of-one-page case remains.  Shipping
  a WAL path that can silently drop runs on crash is unacceptable,
  so the writer waits for B1.3 where the realistic flush-to-run
  (one run per merge, far apart) and a properly-tested redo land
  together.
- Consequently there is no user-facing way to create a second run
  yet; the multi-run scan infrastructure is correct but exercised
  only for the implicit run until the writer returns.

## Subsequent B1 increments (tracked, not built here)

- **B1.2 remainder / B1.3** crash-safe catalog writer +
  pending-flush-to-run + `next_run_id` allocation on flush (creates
  the first durable 2-run state; multi-run scan path exercised
  end-to-end with a from-scratch == multi-run results test).
- **B1.3** Hanoi leveling + tuplesort merge (`merge_strategy.rs`
  port); write-amplification bounded by `max_levels`.
- **B1.4** adaptive collapse to single run under low write
  pressure (`adaptive.rs` mode transitions, tuned for an index AM).
- **B1.5** lazy per-trigram materialization + co-occurrence
  coalescing (the cracking payoff).
