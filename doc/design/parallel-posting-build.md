# Parallel posting-tree build (4.3.1)

Status: implemented in `src/am/ambuild.c`. This note records why it is shaped
the way it is.

## Before

```
participants (leader + W workers): scan -> extract -> worker-role sort  (parallel)
leader:  merge W+1 runs -> for each trigram: posting tree    (serial)
leader:  upper tree, SuRF                                     (serial, small)
```

At large scale the serial middle step dominates. Reading the merged stream
and encoding and writing every posting runs on one core, while the workers
sit idle.

## After

The 64-bit trigram hash space is cut into `R` equal ranges, with `R` = 4 x the
number of planned participants. Each participant feeds `R` worker-role sorts,
one per range, instead of one. Range `r` of every participant lands in
shared sort `r`.

```
participants:  scan -> extract -> route by hash range -> R worker-role sorts
barrier:       every participant has performsort'ed all R sorts
participants:  claim ranges (atomic counter), for each claimed range r:
                 leader-role merge of sort r -> posting trees for range r
                 -> append (hash, root|inline) and SuRF keys to a range file
leader:        wait for all ranges; read range files 0..R-1 in order
               -> upper tree (global hash order = range order), SuRF
```

- **Ordering.** Ranges are contiguous and ascending in hash, so concatenating
  range results 0..R-1 gives exactly the serial `accums[]` order. The upper
  tree and SuRF are identical in content. Only the block numbers of posting
  pages differ, because ranges write interleaved.
- **Leader-role merges.** A tuplesort leader-role merge needs only the shared
  fileset and `nParticipants`, and any backend attached to the DSM can run
  one. `leader_takeover_tapes` checks that all worker runs are finished; the
  barrier guarantees that.
- **Writing pages from workers.** Workers write index pages through
  `pg_tre_extend` (`ExtendBufferedRel`; relation extension locks still
  conflict within a lock group, so extension serialises safely) and
  GenericXLog. Parallel vacuum workers already WAL-log index pages, and
  `RestorePendingSyncs` gives workers the same `RelationNeedsWAL` answer
  under `wal_level=minimal`.
- **Results back to the leader.** BufFiles in a SharedFileSet, one per range,
  as in `sharedtuplestore.c`. Records are
  `(hash u64, root u32, inline_bytes u32, inline bytes…)`, followed by SuRF
  keys.
- **Memory.** Each participant's sort memory (`maintenance_work_mem / P`) is
  split across its `R` sorts. Each range merge runs with
  `maintenance_work_mem / P`.
- **Load balance.** TID volume per trigram is Zipf-skewed, and the hash's
  high bits are not uniform. With one range per participant, the largest
  range ran 50 s past the others on the 2 GB corpus (posting phase 81 s). With
  4 ranges per participant, claimed dynamically, it took 53 s.
  `PG_TRE_RANGES_PER_PARTICIPANT`. Ranges are counted from planned
  participants, so a worker that never launches just leaves its share to the
  others.
- **Serial builds** (no workers, or `pg_tre.enable_parallel_build = off`)
  are unchanged: one sort, leader readout, through the same per-range
  function with a single range.

Not done: sizing ranges from a hash sample. Add it if skew at scale outgrows
the fixed 4x.
