-- Phase B1.4: adaptive collapse.  When runs accrue past the Hanoi
-- level cap (max_levels, default 7), VACUUM collapses them all back
-- into a single base run, bounding growth.  Results must be unchanged
-- and the catalog must return to single-run.
CREATE EXTENSION IF NOT EXISTS pg_tre;

CREATE TABLE col_t(id serial primary key, body text);
INSERT INTO col_t(body)
SELECT md5(g::text) || (CASE WHEN g % 20 = 0 THEN ' findme' ELSE '' END)
FROM generate_series(1, 1000) g;
CREATE INDEX col_idx ON col_t USING tre (body);

-- Accrue many runs (more than max_levels=7) via the test helper.
-- count() forces all 9 function evaluations (no LIMIT short-circuit).
SELECT count(*) AS appended
FROM generate_series(1, 9) g,
     LATERAL tre_debug_append_run('col_idx'::regclass, 0::numeric,
                                  18446744073709551615::numeric) r;

-- > 7 runs now (base + 9 appended).
SELECT CASE WHEN count(*) > 7 THEN 'accrued_many' ELSE 'too_few' END AS before
FROM tre_run_catalog_status('col_idx');

-- VACUUM triggers the collapse (run count > max_levels).
VACUUM col_t;

-- Back to a single run.
SELECT CASE WHEN count(*) = 1 THEN 'collapsed_to_one' ELSE 'still_many' END AS after
FROM tre_run_catalog_status('col_idx');

-- Correctness preserved after collapse: index == seq-scan.
SELECT CASE WHEN
  (SELECT count(*) FROM col_t WHERE body %~~ tre_pattern('findme', 0)) =
  (SELECT count(*) FROM col_t WHERE body LIKE '%findme%')
  THEN 'collapse_correct' ELSE 'collapse_wrong' END AS result;

DROP TABLE col_t;
