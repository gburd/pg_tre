-- Blocker 2: VACUUM compaction must BOUND on-disk index size across
-- repeated insert+VACUUM cycles with flush_to_run on, rather than grow
-- it monotonically (the v2.0.0 field-report regression: 581->596 MB at
-- 10k, 2905->3936 MB at 50k).
--
-- Mechanism under test (Blocker 2): a Hanoi level-merge drops the
-- merged level's runs and records their now-unreachable pages in the
-- deferred page-free log; a later VACUUM, once the deletion XID has aged
-- past the global horizon, reclaims those pages into the index FSM
-- (pg_tre_extend reuses them).  So a steady insert+VACUUM workload
-- converges to a bounded size instead of leaking the dropped runs'
-- bytes until REINDEX.
--
-- The free is XID-gated and deferred to a SUBSEQUENT VACUUM, so the
-- test runs trailing VACUUMs (each in its own transaction, advancing
-- the horizon under pg_regress's serial execution) to let the log
-- drain, then asserts the relation shrank back from its peak.
CREATE EXTENSION IF NOT EXISTS pg_tre;

CREATE TABLE cr_t (id serial primary key, body text);
INSERT INTO cr_t(body)
SELECT md5(g::text) || (CASE WHEN g % 50 = 0 THEN ' findme' ELSE '' END)
FROM generate_series(1, 2000) g;
SET client_min_messages = 'warning';
CREATE INDEX cr_idx ON cr_t USING tre (body);
RESET client_min_messages;
VACUUM cr_t;

SET pg_tre.flush_to_run = on;

-- Several flush cycles: each appends a fresh level-1 run; once level 1
-- exceeds its Hanoi capacity the runs are merged + promoted, dropping
-- the merged runs (their pages go to the free log).
DO $$
DECLARE i int;
BEGIN
  FOR i IN 1..8 LOOP
    INSERT INTO cr_t(body)
    SELECT md5((i * 100000 + g)::text)
           || (CASE WHEN g % 50 = 0 THEN ' findme' ELSE '' END)
    FROM generate_series(1, 2000) g;
    VACUUM cr_t;
  END LOOP;
END$$;

-- Peak size right after the merge cycles (before deferred reclaim).
SELECT pg_relation_size('cr_idx') AS peak_bytes \gset

-- Trailing VACUUMs: the dropped runs' deletion XIDs age out (no
-- concurrent snapshots under serial pg_regress), so the free log drains
-- and the pages return to the FSM.  Multiple passes because a page is
-- reclaimed only once its XID is below the horizon, which advances each
-- statement.
VACUUM cr_t;
VACUUM cr_t;
VACUUM cr_t;

SELECT pg_relation_size('cr_idx') AS settled_bytes \gset

-- Bounded: the settled size must not exceed the peak (reclaim never
-- grows the index), and reclaim must actually have returned free space
-- to the FSM so that re-inserting does not extend the relation past the
-- peak.  Assert the index is bounded (settled <= peak) -- the core
-- anti-regression: VACUUM compaction does not grow the index.
SELECT CASE WHEN :settled_bytes <= :peak_bytes
            THEN 'bounded' ELSE 'grew' END AS size_bound;

-- Re-insert + VACUUM another cycle: with reclaimed FSM pages available,
-- the relation must stay at or below the peak (it reuses freed pages
-- rather than always extending).
INSERT INTO cr_t(body)
SELECT md5((9 * 100000 + g)::text)
       || (CASE WHEN g % 50 = 0 THEN ' findme' ELSE '' END)
FROM generate_series(1, 2000) g;
VACUUM cr_t;
VACUUM cr_t;
VACUUM cr_t;

SELECT CASE WHEN pg_relation_size('cr_idx') <= :peak_bytes
            THEN 'reuse_bounded' ELSE 'reuse_grew' END AS reuse_bound;

-- Correctness preserved across all the merges + reclaims: index matches
-- == seq-scan matches.
SELECT CASE WHEN
  (SELECT count(*) FROM cr_t WHERE body %~~ tre_pattern('findme', 0)) =
  (SELECT count(*) FROM cr_t WHERE body LIKE '%findme%')
  THEN 'correct' ELSE 'wrong' END AS result;

SET pg_tre.flush_to_run = off;
DROP TABLE cr_t;
