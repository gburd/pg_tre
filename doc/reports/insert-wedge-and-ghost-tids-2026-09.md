# INSERT wedge on LWLock:BufferContent, dead TIDs, and pending-list bloat (2026-09)

**Fixed in 4.2.1.** Present since 1.5.5 (the wedge), and since the pending list
existed (the other two).

## Report

A production PostgreSQL 18.6 server with two `tre` expression indexes on a
1.67M-row mail table, upgraded in place to 4.2.0:

- An `INSERT` hung on `LWLock:BufferContent`. Over 19 hours 243 backends and
  the checkpointer queued behind it, `pg_blocking_pids()` was `{}` for every
  one, cancel and terminate did nothing, and connections ran out. After a
  restart the next INSERT wedged again within seconds. Dropping and rebuilding
  both indexes cured it, and the rebuilt indexes were a quarter of the size.
- A separate `tre` index over 12,743 source files (163 MB) was 137 GB, and
  queries through it failed with `could not read blocks 2482..2482 in file
  "...": read only 0 of 8192 bytes`.

The reporter guessed a leaked lock on some early-return path and suspected that
pages written by the older version were involved. Both guesses were reasonable,
and both were wrong.

## 1. The wedge: a backend waiting on itself

`pg_tre_extend_fork` asks the index FSM for a free block, then takes an
**unconditional** exclusive lock on it to check that it is still free. The
function is called while other pg_tre buffers are already held. In
`acquire_tail`, when the pending tail is full, the caller holds the meta page
and the full tail exclusively.

The FSM is not WAL-logged. VACUUM records freed pending pages there, a
checkpoint persists that, and later inserts reuse the pages. The "now used"
mark lives only in shared buffers. After a crash the persisted "free" marks
come back, so the FSM lists pages that are live, and one of them is the current
pending tail. When that tail fills, the backend holds it, extends, gets the
same block back from the FSM, and waits on its own LWLock:

```
#6  LWLockAcquire (mode=LW_EXCLUSIVE)
#7  LockBuffer (mode=2)
#8  pg_tre_extend_fork (kind=PG_TRE_PAGE_PENDING)  src/pages/buffer.c:74
#9  pg_tre_extend
#10 acquire_tail                                   src/pages/pending.c:168
#11 pg_tre_pending_append_batch
#12 pg_tre_aminsert
```

This matches every symptom in the report. The wait is an LWLock, so it is
uninterruptible and invisible to `pg_blocking_pids()`. The backend also holds
the meta page, so every writer and the checkpointer queue behind it. The stale
FSM survives a restart, so the next insert wedges the same way. REINDEX writes
a new relation with a new FSM, which is why it cured the problem.

The layout of the old version's pages played no part. The trigger is a crash
(or `immediate` stop) after VACUUM has freed pages and a checkpoint has
persisted the FSM. The reporter's OS reboot for the upgrade is a plausible
first crash, and each later forced restart recreated the trigger.

**Fix:** `ConditionalLockBuffer`, and skip a block that is busy. nbtree's
`_bt_allocbuf` does the same for the same reason, and its comment describes
this exact own-caller case. A held page is not free; if it really is free, a
later VACUUM records it again.

**Test:** `tap/fsm_stale.pl` builds the stale FSM deterministically. It uses
no checksums and no bgwriter so that nothing flushes or restores the FSM
first. Neither setting is needed in production.

| build | result |
|---|---|
| 4.2.0 | INSERT never returns; `LWLock:BufferContent blockers={}`; stack above |
| 4.2.1 | INSERT completes; index = seq-scan |

## 2. Dead TIDs left in the index; reads past the end of the heap

`ambulkdelete` stripped dead TIDs only from the base posting tree. Take a row
inserted and deleted between two VACUUMs. Its entry is still in the pending
list, so the strip misses it. `amvacuumcleanup` then merges it into the tree,
and no later VACUUM checks it again. Meanwhile the heap frees the line pointer
and can truncate the block. A later scan then fetches a TID past EOF:

```sql
CREATE TABLE st (id serial, body text) WITH (autovacuum_enabled = off);
CREATE INDEX ON st USING tre (body);
INSERT INTO st (body) SELECT 'ghost_' || g || repeat(' pad', 40)
  FROM generate_series(1, 5000) g;
DELETE FROM st;
VACUUM st;                           -- heap truncates to 0 blocks
INSERT INTO st (body) VALUES ('alive');
SET enable_seqscan = off;
SELECT count(*) FROM st WHERE body ~ 'ghost_4';
-- 4.2.0: ERROR: could not read blocks 1..1 in file "base/5/16440":
--        read only 0 of 8192 bytes
-- 4.2.1: 0
```

That is the report's read error. If the line pointer is reused instead of
truncated away, the scan returns another row to recheck. Results stay correct,
but the index never sheds the TID. `pg_tre.flush_to_run` had the same leak by a
second route: its merges land in catalog runs, which the strip never visited.

**Fix:** as `ginbulkdelete` does, merge the pending list before stripping, and
walk every live run instead of only the base tree. Roots shared between runs
are walked once.

**Test:** `test/sql/vacuum_pending_dead.sql` covers both routes. 4.2.0 fails
both. A control build with only the merge-first half still fails the
`flush_to_run` case.

## 3. One pending entry per trigram occurrence

`aminsert` appended a 24-byte entry for every trigram occurrence. Nothing reads
the stored position: both the merge and the scan overlay fold repeats into one
posting TID. The bulk build already dedups per row. Source text repeats its
trigrams about 9x, so the pending list reached roughly 24x the text it indexed,
and every scan reads the whole list until VACUUM merges it.

**Fix:** a per-row open-addressing set. It lives on the stack for ordinary
rows and is capped at 32 MB for huge ones, past which the row stops deduping
rather than grow the set.

## 4. Unbounded growth under an old snapshot (warned, not changed)

Each merge rewrites the posting tier, and the old copy is freed once no
snapshot can still see it. A snapshot held across N merges therefore pins N
copies. This is correct behaviour, the same rule nbtree follows, but it was
silent. It is the only mechanism we found that multiplies size without bound,
so it is the likeliest explanation for 137 GB over 163 MB, together with #3.
VACUUM now raises a `WARNING` when pages freed by an **earlier** VACUUM, still
held back, make up at least half the index and 128 MB.

## Measurements

EC2 `c7i.4xlarge`, Debian 12, gcc 12.2, PostgreSQL 18.6 (`-O2`),
`shared_buffers=4GB`. Rows are inserted into an already-indexed table, then
VACUUMed. The two builds alternate, three runs each, and medians are reported.

| workload | build | INSERT | index after INSERT | VACUUM | index after VACUUM | query p50 | fresh CREATE INDEX |
|---|---|---|---|---|---|---|---|
| 200,000 short rows (mail-subject shape) | 4.2.0 | 2.24 s | 350 MB | 16.7 s | 508 MB | 24.3 ms | 17.0 s, 157 MB |
| | 4.2.1 | 2.21 s | 350 MB | 16.4 s | 508 MB | 22.0 ms | 16.4 s, 157 MB |
| 3,749 source files (62 MB) | 4.2.0 | 12.5 s | 1,504 MB | 74.2 s | 1,601 MB | 20.0 ms | 4.4 s, 94 MB |
| | 4.2.1 | 2.9 s | 165 MB | 15.0 s | 260 MB | 19.5 ms | 4.4 s, 94 MB |

Short rows rarely repeat a trigram, so #3 changes nothing for them. The
first 4.2.1 short-row run (5.3 s) is a cold-cache outlier, and the other two
agree with 4.2.0. The fresh build is unchanged: it already deduped.

Under one held `REPEATABLE READ` snapshot, six ingest+VACUUM rounds of 11,247
source files grew the index from 140 MB to 1.6 GB against 75 MB of heap. Once
the snapshot ended, all 83,574 held pages went back to the FSM.

## What an existing deployment should do

- Install 4.2.1. The wedge cannot recur once the fixed library is loaded, and
  no REINDEX is needed for that.
- **REINDEX** any `tre` index that has shown a `could not read blocks` error,
  or that is far larger than a fresh build would be. The fix stops new dead
  TIDs but does not remove ones an earlier version already merged in.
- If VACUUM warns about old snapshots, find and end the long-lived session
  (`pg_stat_activity.backend_xmin`, `pg_prepared_xacts`, replication slots).

## Not reproduced

We did not reproduce 137 GB, nor the exact block number in the reported read
error. Mechanisms #2, #3 and #4 each reproduce a piece of it, and together they
account for the shape. We found no mechanism in the build path: a fresh
`CREATE INDEX` over the same 11,247 files is 269 MB and agrees with a seq-scan.
