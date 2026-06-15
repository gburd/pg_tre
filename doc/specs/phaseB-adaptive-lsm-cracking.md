# Phase B — adaptive-LSM cracking substrate

> **North star: pg_tre is a REGEX index with edit distance.**
> Phase B changes *how* the index is built and maintained — from a
> build-it-all-up-front structure to an adaptive, incrementally
> merged ("cracked") one — so the single-extension story is viable
> on large corpora and on machines with less RAM than a full build
> needs.  It does not change what pg_tre matches.

**Status:** draft.  **Targets:** B1 → a 2.0 line (on-disk format
change); B2 → a later 2.x once B1 is solid.  **Depends on:**
Phase A is independent and should ship first.

## B0. The model, and why LSM fits where pattern-cracking didn't

Classic database cracking (MonetDB) self-organizes an index as a
byproduct of queries *because the cracked unit — a range pivot on
a sortable column — composes*: each query refines the partition,
converging toward sorted.  Edit-distance/regex *patterns* do not
compose (the answer to `foo{~1}` tells you nothing reusable about
`bar{~2}`), so cracking at the *pattern* level would be a result
cache wearing an index's clothes.

**pg_tre's composable unit is the trigram.**  So we crack on
trigrams, and the right mechanism is an **adaptive LSM** modeled
on `~/ws/aether/src/lsm`:

- New rows land in an in-memory **nursery (L0)** — pg_tre already
  has this as the `fastupdate` pending list.
- L0 flushes to a sorted **run**; runs accrete into **levels**
  with **Hanoi-tower** capacities (level N holds 2^(N-1) runs),
  giving bounded write amplification (aether `hanoi.rs`).
- Merges are **overwrite-on-merge with newest-wins + tombstones**
  (aether `query.rs` merge semantics) — never in-place update.
- Crucially, aether's **adaptive collapse**: under low write
  pressure the structure reduces from MultiLevel → Hybrid →
  SingleIndex (aether `adaptive.rs`).  A quiescent pg_tre index
  collapses toward **one clean run** — i.e. it converges to the
  current single-structure index, getting LSM benefits only while
  they're earned.

This is the elegant core: **the index is a set of layers each
owning a portion of the (trigram) keyspace; ingest batches in the
nursery and becomes a new layer; merges collapse layers back down
when the workload allows.**

### Mapping aether → pg_tre

| aether | pg_tre |
|---|---|
| `WriteBuffer` (L0, BTreeMap) | the `fastupdate` pending list (already exists) |
| `Run<I>` (sorted run + SuRF filter) | a sorted posting-tree segment + its per-run trigram bloom/range summary |
| `Level<I>` (runs by `RunId`) | a pg_tre level: a set of runs covering trigram ranges |
| `HanoiStrategy` | the merge-trigger policy (port directly) |
| newest-wins + `TOMBSTONE` | newest-run-wins on (trigram, TID); tombstone = deleted TID |
| `AdaptiveLsm` mode transitions | MultiLevel ↔ Hybrid ↔ Single collapse |
| `Surf` range filter | per-run trigram-range + bloom skip filter |

The backing per-run structure is pg_tre's existing posting-tree +
upper-tree + range-tier layout (format v6), so a run is "a small
pg_tre index over a slice of recently-ingested rows."

## B1 — 1-D adaptive LSM (incremental build, no visibility axis)

The conservative, correctness-preserving first step.  Keeps the
normal `amgetbitmap` → bitmap-heap-scan → **executor recheck**
contract; the LSM only changes how index data is organized and
grown.

### What changes

1. **L0 = pending list, formalized.** Inserts append to the
   nursery (as today).  When it exceeds a threshold it flushes to
   a run rather than merging into one monolithic structure.
2. **Runs + Hanoi leveling.** `pg_tre_pending_merge` becomes a
   run-flush + level-merge driver using the ported `HanoiStrategy`.
   Each merge sorts via tuplesort (1.8.0 machinery) so merge
   memory stays bounded by `maintenance_work_mem`.
3. **Scan merges across runs.** `amgetbitmap` unions the candidate
   sparsemaps from all runs (newest-wins on (trigram,TID),
   tombstones subtract deleted TIDs), exactly like aether's
   `merge_range`.  Per-run trigram-range/bloom filters skip runs
   that can't contain a queried trigram (aether `Surf`).
4. **Adaptive collapse.** Background (or VACUUM-driven) compaction
   collapses levels when write pressure is low, converging to a
   single run — at which point scan cost equals today's.
5. **Lazy per-trigram materialization (the "cracking" payoff).**
   A trigram's posting data need not be fully built until first
   queried: a cold trigram can live only in the newest run(s); on
   first query touching it, optionally promote/coalesce its
   postings across runs (a "crack").  Your item (d) —
   opportunistically coalesce co-occurring trigrams of the rows we
   touch while we're there.

### Why B1 is safe

- **Stores keys, not answers.**  Still (trigram → TID); the
  executor still rechecks visibility against the heap.  No MVCC
  contract is touched.
- **Crash/replica behavior unchanged in kind.**  Runs are WAL'd
  like today's pages; a standby replays them; no read-side writes.
- **Worst case = today.**  A fully-collapsed index is the current
  single-structure index.

### B1 deliverables

- On-disk: a run/level catalog in the meta page (run ids, level
  assignment, per-run trigram-range summary). Format bump.
- `merge`/compaction driver with ported Hanoi policy + adaptive
  collapse thresholds (from aether `adaptive.rs`, tuned for an
  index AM not a KV store).
- Multi-run `amgetbitmap` with run-skip filters.
- Tests: ingest-heavy workload builds levels; quiescence collapses
  to one run; scan results identical to a from-scratch build;
  build/merge memory bounded by `maintenance_work_mem`;
  crash-recovery and replication parity (existing shell tests
  extended for multi-run state).

## B2 — visibility-aware 2-D LSM (the research bet)

Only after B1 is solid.  Adds a **second LSM dimension keyed on
visibility** so the index can memoize candidate result-sets and
prune whole runs by snapshot horizon — making pg_tre the first AM
to keep stored answers valid across snapshots *in harmony with*
the executor, rather than relying on per-row recheck.

### The 2-D structure

A run is bounded on two axes:

- **keyspace axis** — which trigram range it owns (as in B1).
- **visibility axis** — the commit-LSN / full-transaction-id
  window over which its entries are valid.

Each chunk encodes the visibility metadata needed to (a) select
the right runs for a scan's snapshot and (b) drop the correct
portions on VACUUM.

### Critical correctness constraints (these gate the whole phase)

1. **Wraparound safety.** PostgreSQL `TransactionId` is 32-bit and
   **wraps**; aether's `RunId` is cleanly monotonic and does not.
   The visibility axis MUST use 64-bit `FullTransactionId` /
   `XLogRecPtr` (commit LSN), never bare `xid`.  A wraparound bug
   here yields *wrong answers*, the worst class.  **Requires a
   written invariant + proof before any code.**
2. **Snapshot scoping, not recheck-skipping.**  Be precise: the
   executor's visibility contract is not bypassed.  What B2 buys
   is *run pruning* — runs whose visibility window is entirely
   outside the scan's snapshot are skipped, and the candidate set
   from surviving runs is smaller.  Correctness still ultimately
   rests on the heap; B2 makes the index *cheaper and snapshot-
   aware*, it does not make stale answers correct.
3. **Standby behavior.**  Reads on a hot standby cannot write new
   runs.  The memoization/crack-on-read path must be a no-op (or
   advisory-only) on a standby; the index must remain correct and
   simply not self-optimize there.  Must be specified explicitly.
4. **Crash recovery.**  Nursery/in-flight runs and the
   memoization layer must either be WAL'd or be reconstructable
   and treated as cold after crash/failover — never assumed
   durable-and-warm.

### VACUUM integration

`amvacuumcleanup`/`ambulkdelete` drop visibility slices below the
freeze horizon and tombstone dead TIDs; the 2-D merge collapses
survivors.  This is where "remove the correct portions of the
index on vacuum" is realized — and it's why the visibility
metadata must be encoded per chunk.

### Why B2 is gated

It fights two PostgreSQL contracts at once (MVCC visibility +
read-only replicas/WAL) and adds 2-D merge-policy complexity on
top of a 32-bit-wrapping clock.  It is high-novelty, high-reward,
and high-risk.  It does not start until B1 ships and qualifies,
and it does not merge without the four constraints above answered
in writing with tests.

## Sequencing summary

1. **Phase A** (independent, ship first): capability parity.
2. **Phase B1**: 1-D adaptive LSM; incremental, memory-bounded,
   self-collapsing build; executor contract untouched.
3. **Phase B2**: 2-D visibility axis; snapshot-scoped run pruning
   + memoization; gated on wraparound/standby/crash proofs.

## Explicitly out of scope (restating, because LSMs invite it)

- No general KV store.  The LSM is internal substrate for a
  trigram index, not a user-facing store.
- No BM25 / TF-IDF / ranked full-text.
- No result cache that returns answers without the executor's
  visibility check (B2 prunes runs by snapshot; it does not return
  unchecked answers).
