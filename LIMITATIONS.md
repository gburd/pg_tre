# pg_tre — Known Limitations and Production Sizing

This file captures **honest, evidence-based limits** of pg_tre as
deployed.  It exists because the previous documentation set
implied production-readiness at scales where the build path is
not viable, and an operator hit it the hard way (see "Field
report" below).

---

## TL;DR

| dataset shape | safe? | notes |
|---|---|---|
| ≤ 100k rows of long text (e.g., email bodies, ~1 KB) | ✅ | typical peak build RSS ~1–2 GB |
| ≤ 500k rows of short text (e.g., subjects, identifiers, ~50 chars) | ✅ | typical peak build RSS ~600 MB – 1 GB |
| 100k–500k rows of long text | ⚠️ | peak build RSS often 2–6 GB; budget `maintenance_work_mem` and pg_tre's working set explicitly |
| ≥ 500k rows of long text | ❌ | builds frequently OOM-kill the postgres cgroup with default settings; see "Sizing the build" below before attempting |
| ≥ 1M rows of any natural text | ❌ | not currently viable in 1.6.x — the build is in-memory and grows with total trigram emissions, not with distinct trigrams |

For workloads above the green band, **pair pg_tre with pg_trgm
(GIN) and `tsvector` BM25 instead of using pg_tre as the only
text index.** That is the documented design pattern several
production deployments are running today (see
`README.md#deployment-recommendations`).

---

## Sizing the build (memory model)

pg_tre's `ambuild` is currently fully in-memory.  Peak resident
working set is dominated by three components:

```
peak_RSS ≈
    ( emitted_trigrams_per_row × N_rows × 24 B )      -- entries[]
  + ( N_rows × ~56 B )                                -- tid_blooms hashtable + per-row bloom
  + ( ~hundreds of MB fragmentation in pass B )
  + ( shared_buffers + work_mem + libpq + ... )       -- the rest of the backend
```

Concretely, with default settings:

| corpus | trigrams/row | rows | entries[] | total build RSS (typical) |
|---|---|---|---|---|
| URLs / IDs (~50 chars) | ~50 | 500k | 600 MB | ~1 GB |
| short subjects | ~80 | 500k | 1 GB | ~1.5 GB |
| email bodies (~1 KB) | ~500 | 500k | **6 GB** | **~8 GB** |
| email bodies (~1 KB) | ~500 | 100k | 1.2 GB | ~2 GB |
| long documents (~10 KB) | ~5000 | 100k | 12 GB | **OOM under any reasonable cgroup** |

The build can also retain GBs of pass-B `palloc` fragmentation
even after individual entries are freed; cgroup memory accounting
sees the high-water mark regardless.

`pg_tre.min_trigram_freq` (default 1, no skip) can be raised to
drop posting trees for trigrams below the threshold, which
reduces output index size but **does not reduce build memory** —
all emissions are still collected before the skip decision is
made.

A fix is planned: a `tuplesort`-based build (the same disk-spill
pattern btree/gin/gist use) that bounds working set by
`maintenance_work_mem` instead of by dataset size.  Tracked for
**v1.8.0**.  In the meantime, the sizing table above is what to
plan for.

## Cancellability

Until v1.7.0, three phases of `ambuild` are uncancellable for
seconds-to-minutes at a time:

- the `pg_qsort` over the entries array (typically 5–60s),
- the `sm_add_grow` per-TID loops inside `pg_tre_posting_build_finish`,
- the multi-leaf write paths.

**`REINDEX` on a pg_tre index holds heavy locks (`AccessExclusiveLock`
on the index, blocking writes via `RowExclusiveLock` on the heap)
during the entire build**, so a build that takes 6 minutes blocks
that table's writes for 6 minutes.  Cancelling with
`pg_cancel_backend()` or SIGINT is honored only at the next
`CHECK_FOR_INTERRUPTS` boundary.

`CREATE INDEX CONCURRENTLY` and `REINDEX CONCURRENTLY` are not yet
supported by the AM (`amcanbuildconcurrently = false`); attempting
either falls back to the lock-heavy form.  Both are tracked for
**v1.7.0**.

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

The two failure modes map directly to v1.7.0 (cancellability +
bounded-error build) and v1.8.0 (tuplesort-based build).  Both
are committed work, but if your workload looks like Agora's,
plan as if pg_tre is small/medium-corpus territory until 1.8.0
ships and qualifies on a real >500k-row body corpus.
