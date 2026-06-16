# pg_tre — Known Limitations and Production Sizing

This file captures **honest, evidence-based limits** of pg_tre as
deployed.  It exists because the previous documentation set
implied production-readiness at scales where the build path is
not viable, and an operator hit it the hard way (see "Field
report" below).

---

## TL;DR
> **As of v1.8.0 the build is no longer in-memory** — it sorts
> trigram tuples with PostgreSQL's `tuplesort`, so peak build
> memory is bounded by `maintenance_work_mem` (disk-spilled)
> instead of by corpus size.  The OOM behavior described in older
> revisions of this file is fixed.  The table below now reflects
> 1.8.0; for 1.6.x/1.7.x behavior see the git history.

| dataset shape | safe? | notes |
|---|---|---|
| ≤ 100k rows of long text (e.g., email bodies, ~1 KB) | ✅ | builds memory-safely; **but check temp disk first** — ~64 B per trigram occurrence (run `tre_estimate_index_build`) |
| ≤ 500k rows of short text (~50 chars) | ✅ | modest temp footprint; fine on a normal host |
| 100k–500k rows of long text | ⚠️ | builds memory-safely, but build **temp disk** can reach tens of GB on long text — size it with `tre_estimate_index_build` before starting |
| ≥ 500k rows of long text (multi-GB column) | ❌ (today) | memory-safe and cancellable, but build-time **temp-disk** is the wall: a production user measured ~21 GB temp at 2.1 % of a body corpus — hundreds of GB extrapolated. Pair with `pg_trgm` until 2.0's de-dup lands |
| ≥ 1M rows of any natural text | ❌ (today) | same temp-disk wall; also query-time selectivity / index size questions |

The gating factor at large scale is **build-time temp disk**, not
memory (memory has been bounded by `maintenance_work_mem` since
1.8.0).  A production user reported the index AM unusable on a
multi-GB text column at every shipped version for the temp-disk
reason; see the sizing section and field report below.  Pairing
pg_tre with `pg_trgm`
(GIN) for substring/`LIKE` and `tsvector` BM25 for ranked
full-text, reserving pg_tre for true edit-distance/regex fuzzy
matching, remains the recommended pattern for the largest
text-search workloads (see
`README.md#deployment-recommendations`).

---

## Sizing the build (memory model)

As of v1.8.0 `ambuild` sorts trigram tuples via `tuplesort`, so
**peak build memory is bounded by `maintenance_work_mem`** plus a
fixed per-backend overhead — it does not grow with the number of
trigram emissions.  Measured on a body corpus under
`maintenance_work_mem = 32MB`:

| corpus | emissions | peak build-backend RSS |
|---|---|---|
| 100k rows, ~395-char bodies | ~40 M | 219 MB |
| 200k rows, ~395-char bodies | ~80 M | 231 MB |

Peak RSS is essentially flat across a 2x corpus (the ~12 MB delta
is the tid-bloom hash, which is O(distinct TIDs)); the former
in-memory build would have grown from ~960 MB to ~1.9 GB of
resident `entries[]` alone.  Raise `maintenance_work_mem` to trade
RAM for fewer merge passes and faster builds.

What *does* still scale with corpus size:

```
temp_disk    ≈ emitted_trigrams × ~64 B    (tuplesort spill, real cost)
index_size   ≈ distinct_trigrams × ~16 B   (after sparsemap TID-list compression)
tid_bloom    ≈ N_rows × ~56 B              (resident; O(distinct TIDs))
```

**The temp-disk figure is the one that bites.** Each trigram
*occurrence* in the text becomes one `tuplesort` entry; the encoded
key is 24 bytes but tuplesort wraps every datum in a
`SortTuple`+`MinimalTuple`, so the real on-disk cost is **~64 bytes
per emitted tuple**. Natural text emits roughly one trigram per
character, so:

```
temp_disk ≈ (bytes_of_text) × ~64 B/char     (worst case, no repetition)
```

**Field data point (production, v1.8.2):** a user indexing message
bodies measured **~21 GB of build temp at 2.1 % of their body
corpus** — consistent with ~64 B per emission over hundreds of MB
of text. Extrapolated, a multi-GB text column needs **hundreds of
GB of build-time temp disk**. If you do not have that, the build
will not complete; size it up front.

**Size it before you start.** As of 2.0 call:

```sql
SELECT * FROM tre_estimate_index_build('your_table'::regclass, attno);
--  sample_rows | est_rows | est_trigrams | est_temp_mb | est_index_mb
```

It samples up to 2000 rows of the column and extrapolates the
emission count, build temp-disk, and final index size. Set
`pg_tre.build_max_entries_mb` from `est_temp_mb` (the GUC uses the
same ~64 B/tuple cost), or decide the column is too large and pair
with `pg_trgm` instead.

The `pg_tre.build_max_entries_mb` GUC (default 0 = unlimited
since 1.8.0) caps build temp-disk: when emitted-tuple count × ~64 B
would exceed it, the build fails cleanly with
`ERRCODE_PROGRAM_LIMIT_EXCEEDED` (cancellable, never OOM) rather
than filling the temp tablespace. As of 2.0 the ceiling is
measured against the real ~64 B/tuple cost, not the bare 24 B key,
so it tracks actual temp consumption.

> **Roadmap (2.0):** per-row de-duplication of repeated trigrams
> collapses the dominant cost — repetitive text (a body that
> mentions the same words many times) emits one tuple per
> *distinct* trigram per row instead of one per *occurrence*,
> cutting temp-disk several-fold on natural text. Tracked for the
> 2.0 line.

## Cancellability

As of v1.7.0 the build is cancellable throughout, and v1.8.0's
`tuplesort_performsort` is itself interruptible; the per-TID
`sm_add_grow` loops and multi-leaf write paths carry
`CHECK_FOR_INTERRUPTS`.  A `pg_cancel_backend()` / SIGINT during
a build is honored within a second or so on any phase.  (Before
1.7.0 the `pg_qsort` over the in-memory entries array — 5–60s on
a large corpus — and the posting-build loops were
uncancellable.)

**Avoid the heavy-lock build entirely**: plain `REINDEX` takes
`AccessExclusiveLock` on the index and blocks the table's writes
for the whole build (this is what stalled ingest for 6 minutes
in the field report below).  Use the CONCURRENTLY variants
instead — they are **supported and verified** (CIC/RIC are
generic in PostgreSQL; pg_tre's build works correctly under the
two-phase MVCC-snapshot protocol, confirmed by a CIC-result ==
seq-scan-ground-truth cross-check):

```sql
CREATE INDEX CONCURRENTLY my_tre ON t USING tre (body);
REINDEX INDEX CONCURRENTLY my_tre;
```

CONCURRENTLY builds are slower in wall-clock but do not block
concurrent reads/writes on the table, and (since 1.8.0) are
memory-bounded by `maintenance_work_mem` like any other build.

## Format-version upgrades

A pg_tre format-version bump (e.g., 1.5.x → 1.6.0's v5 → v6)
requires `REINDEX`, but the REINDEX may itself be unviable per
the "Sizing the build" section above.  In that case the safe
operator path is:

1. `DROP INDEX … RESTRICT;` the pg_tre index
2. `ALTER EXTENSION pg_tre UPDATE TO 'X.Y.Z';`
3. Decide whether the column actually warrants pg_tre at all (see
   the table at the top), or whether `pg_trgm` GIN + `tsvector`
   BM25 covers the workload.
4. If still wanted, recreate the index during a maintenance
   window with `maintenance_work_mem` set generously and the
   build memory budget calculated from the table above.

The reader (since v1.6.0) rejects pre-format-v6 pages with
`ERRCODE_FEATURE_NOT_SUPPORTED` and a `REINDEX` hint, so an
un-rebuilt index fails loudly on first scan rather than returning
wrong rows — there is no silent-corruption window.

## Field report — 2026-06 (Agora deployment)

A production user (501 k rows, email-archive schema) hit two
distinct failures while attempting the 1.6.0 REINDEX:

> 1. `idx_ag_msg_from_tre` (501 k rows): the reindex hung 6 min
>    holding a lock that stalled mail ingestion.  I cancelled it.
>    It was invalid + idx_scan=0 (unused) + from_addr is pg_trgm
>    by design — so I dropped it.
> 2. Building the body tre index OOM-killed the entire postgres
>    cgroup (auto-restarted, replication survived).  v1.6.0 fixes
>    the data-loss overflow (32→64-bit offset) — but that only
>    removes the fast error, so the build now grinds until it
>    exhausts memory and crashes the server.

Their conclusion ("subject/from_addr use pg_trgm GIN, body uses
BM25; pg_tre indexes are not viable at this scale") is correct
for v1.6.x.  Their schema design note had already documented this
pattern internally; the gap was that pg_tre's own README/
CHANGELOG implied the indexes were production-ready.  This file
exists to close that gap.

The two failure modes are addressed as follows: the **lock stall
is avoidable today** with `REINDEX INDEX CONCURRENTLY` /
`CREATE INDEX CONCURRENTLY` (supported and verified), and v1.7.0
adds full build cancellability plus the
`pg_tre.build_max_entries_mb` ceiling so the OOM becomes a clean
error.  v1.8.0's `tuplesort`-based build is the structural fix
that lets large body corpora build within `maintenance_work_mem`.
Until 1.8.0 ships and qualifies on a real >500k-row body corpus,
plan as if pg_tre is small/medium-corpus territory for long-text
columns, per the table at the top.
