# Denser TID packing for posting sparsemaps (implemented in 4.4.0)

## Problem

A pg_tre index over long documents is several times larger than pg_trgm's
GIN index on the same column:

| corpus | rows | text | tre (4.3.1) | pg_trgm GIN |
|---|---|---|---|---|
| depesz, explain.depesz.com plans (blog, 4.x) | 1.6 M | 33 GB | 21 GB | 1.67 GB |
| synthetic plans (EC2, 2026-10) | 160 k | 2.7 GB | 1349 MB | 358 MB |
| synthetic prose, 2 GB subset | 702 k | 2.1 GB | 7326 MB | 1040 MB |

Postings encode the TID packed as `(block << 16) | offset` in a sparsemap.
A sparsemap chunk covers 2048 bits, but one heap block takes 65536 bits.
So every heap block a posting touches pays for a full chunk (start offset,
descriptor, bit vector), however few of its rows are in the posting. Long
documents leave only about 1–14 rows per heap page; depesz's table has
about 1.7. Measured with the vendored sparsemap (a standalone encoder over
synthetic postings, 2026-10):

| rows/block | density | `<<16` B/TID | `blk*292+off` B/TID |
|---|---|---|---|
| 1 | 0.9 | 24.0 | 10.5 |
| 4 | 0.9 | 6.7 | 3.0 |
| 4 | 0.3 | 15.2 | 7.0 |
| 30 | 0.3 | 2.7 | 1.5 |
| 120 | 0.05 | 5.2 | 3.4 |

## Proposal

Pack TIDs as `block * MaxHeapTuplesPerPage + offset`, i.e. 291 + 1 for 8 KB
pages and 32-bit blocks. The packed value stays below 2^41. Sort order is
unchanged (the mapping is monotonic), so the build, merge and scan
algorithms stay as they are. This alone gives 2–2.3x smaller postings on
long-document tables, and therefore fewer pages to build, WAL-log and read.

This is an on-disk format change:

- record the packing in the meta page (new field, carved from `reserved[]`;
  0 = legacy `<<16`). Every pack/unpack (about 20 sites: build, scan,
  pending list, vacuum, posting leaf `min_tid`/`max_tid`) reads it through
  the relation;
- new builds use the dense packing. Old indexes keep working unchanged, and
  `REINDEX` (or `pg_tre_upgrade_index`) converts them;
- bump `PG_TRE_FORMAT_VERSION_LATEST`, so an older binary refuses a new
  index instead of misreading it;
- tests: the CI in-place upgrade job from 4.3.x, a mixed-packing pending
  list, VACUUM across both packings, and a seq-vs-index differential on an
  old-format index after the upgrade.

Beyond that, a per-chunk sparse encoding tuned for 1–2 set bits per block
would close most of the remaining gap to GIN's varbyte posting lists. Measure
it after this change.

## As built (4.4.0)

- `PgTreMetaPageData.tid_packing` (`PgTreTidPacking`: 0 legacy, 1 dense),
  read once per relcache entry into `rd_amcache` by `pg_tre_tid_packing()`
  (`src/pages/meta.c`) without the content lock -- the field never changes
  after the relfilenode is created, and the caller may already hold the
  meta buffer. `pg_tre_build_empty_fork()` writes it and primes the cache.
- The stride is `MaxHeapTuplesPerPage + 1` (292 for 8 kB pages); packing
  an offset at or above it raises an ERROR instead of aliasing the next
  block.
- The build's sort key keeps legacy packing (lossless for any offset, same
  order) and the posting builder repacks.
- `pg_tre_upgrade_index()` does not convert; it points a legacy index at
  `REINDEX`. A non-heap table AM is built legacy.
- `test/sql/tid_packing.sql` runs the same rows through both packings.

Measured (PostgreSQL 18.6, FreeBSD 15.1, i4i.8xlarge; 4 GB
`maintenance_work_mem`, 8 participants):

| corpus | rows / heap block | 4.3.1 | 4.4.0 | ratio | pg_trgm GIN |
|---|---|---|---|---|---|
| 4.1 GB prose, 1.4 M rows | 8.9 | 14.2 GB | 8.0 GB | 1.77x | 2.0 GB |
| 2.5 GB EXPLAIN plans, 160 k rows | 13.6 | 1349 MB | 886 MB | 1.52x | 358 MB |
| 1.3 GB prose kept in the heap, 300 k rows | 1.4 | 9.4 GB | 5.5 GB | 1.71x | 864 MB |

The ratios fall short of the 2–2.3x the synthetic postings predicted: a
real corpus's postings mix dense trigrams (many rows per block, where both
packings already pack well) with sparse ones. Build time is unchanged
(757 s vs 786 s at 64 MB / 1 participant on the prose corpus, 319 s vs 336 s
at 4 GB / 8). Average sparsemap bytes per TID on a full posting leaf went
from 9.4 to 5.3 on the prose corpus.
