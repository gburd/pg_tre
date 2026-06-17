-- Phase B1.3: pending-flush-to-run (the production multi-run path).
-- With pg_tre.flush_to_run=on, VACUUM flushes the pending list into a
-- NEW catalog run instead of merging into the base structure.  The
-- multi-run scan must union base + run(s) and return exactly the same
-- rows as a seq scan.  Assertions use single tokens to stay robust.
CREATE EXTENSION IF NOT EXISTS pg_tre;

CREATE TABLE ftr_t(id serial primary key, body text);
INSERT INTO ftr_t(body)
SELECT md5(g::text) || (CASE WHEN g % 25 = 0 THEN ' findme' ELSE '' END)
FROM generate_series(1, 1000) g;
CREATE INDEX ftr_idx ON ftr_t USING tre (body);

SET pg_tre.flush_to_run = on;
INSERT INTO ftr_t(body)
SELECT md5((g + 100000)::text) || (CASE WHEN g % 25 = 0 THEN ' findme' ELSE '' END)
FROM generate_series(1, 1000) g;
VACUUM ftr_t;

-- At least 2 runs after the flush (base + flushed run).
SELECT CASE WHEN count(*) > 1 THEN 'multiple_runs' ELSE 'single_run' END AS runs
FROM tre_run_catalog_status('ftr_idx');

-- Correctness: index matches == seq-scan matches across all runs.
SELECT CASE WHEN
  (SELECT count(*) FROM ftr_t WHERE body %~~ tre_pattern('findme', 0)) =
  (SELECT count(*) FROM ftr_t WHERE body LIKE '%findme%')
  THEN 'multirun_correct' ELSE 'multirun_wrong' END AS result;

SET pg_tre.flush_to_run = off;
DROP TABLE ftr_t;
