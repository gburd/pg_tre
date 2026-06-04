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

| dataset shape | safe? | notes (1.8.0) |
|---|---|---|
| ≤ 100k rows of long text (e.g., email bodies, ~1 KB) | ✅ | builds under any reasonable `maintenance_work_mem` |
| ≤ 500k rows of short text (~50 chars) | ✅ | same |
| 100k–500k rows of long text | ✅ | set `maintenance_work_mem` to taste (more = fewer merge passes); peak RSS stays bounded, not GBs |
| ≥ 500k rows of long text | ✅ | builds memory-safely; use `CREATE INDEX CONCURRENTLY` to avoid the build lock; expect temp-disk use proportional to total trigram emissions |
| ≥ 1M rows of any natural text | ⚠️ | builds memory-safely, but query-time selectivity and index size are the real questions — see Performance and the pairing advice below |

Memory is no longer the gating factor.  The remaining reasons to
prefer the pairing below at very large scale are **query latency**
and **index size**, not build OOM.  Pairing pg_tre with `pg_trgm`
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
temp_disk    ≈ emitted_trigrams_per_row × N_rows × 24 B   (tuplesort spill)
tid_bloom    ≈ N_rows × ~56 B                            (resident; O(distinct TIDs))
index_size   ≈ grows with distinct trigrams + per-tuple bloom payload
```

The `pg_tre.build_max_entries_mb` GUC (default 0 = unlimited
since 1.8.0) is an optional cap on the emitted-tuple count, i.e.
a *temp-disk* safety valve — set it if you want the build to fail
cleanly with `ERRCODE_PROGRAM_LIMIT_EXCEEDED` rather than fill
the temp tablespace.

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
