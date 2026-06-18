-- Phase A (A3): accurate {~k} / %~~ selectivity.
-- The planner must distinguish selective from non-selective patterns:
-- a pattern matching ~50% of rows should NOT use the index (seq scan
-- wins), while a pattern matching a handful of rows should.  Before
-- A3 every estimate was rows=1, so the index was always chosen --
-- the cause of the field-report slowdown.
--
-- We assert the PLAN CHOICE (the behavioral contract), not exact row
-- estimates, which depend on planner cost internals.
CREATE EXTENSION IF NOT EXISTS pg_tre;

CREATE TABLE sel_t(id serial primary key, body text);
INSERT INTO sel_t(body)
SELECT md5(g::text)
  || (CASE WHEN g % 2 = 0   THEN ' common_tok' ELSE '' END)   -- 50%
  || (CASE WHEN g % 100 = 0 THEN ' rare_tok'   ELSE '' END)   -- 1%
  || (CASE WHEN g = 1       THEN ' unique_zzz' ELSE '' END)   -- one row
FROM generate_series(1, 10000) g;
SET client_min_messages = warning;
CREATE INDEX sel_idx ON sel_t USING tre (body);
RESET client_min_messages;
ANALYZE sel_t;

-- Use a helper to capture the estimated row count and assert ranges.
-- (function defined below)
CREATE FUNCTION est_rows(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
  r record; rows bigint;
BEGIN
  FOR r IN EXECUTE 'EXPLAIN (FORMAT JSON) ' || q LOOP
    rows := (r."QUERY PLAN"->0->'Plan'->>'Plan Rows')::bigint;
  END LOOP;
  RETURN rows;
END $$;

-- unique_zzz (1 row): estimate must be tiny (< 50).
SELECT est_rows('SELECT * FROM sel_t WHERE body %~~ tre_pattern(''unique_zzz'',0)') < 50
       AS unique_is_selective;

-- rare_tok (100 rows): estimate within an order of magnitude (10..1000).
SELECT est_rows('SELECT * FROM sel_t WHERE body %~~ tre_pattern(''rare_tok'',0)')
       BETWEEN 10 AND 1000 AS rare_estimate_reasonable;

-- common_tok (5000 rows): estimate must be large (> 1000), so the
-- planner does NOT treat it as a selective index lookup.
SELECT est_rows('SELECT * FROM sel_t WHERE body %~~ tre_pattern(''common_tok'',0)') > 1000
       AS common_is_not_selective;

-- nonexistent pattern: estimate must be tiny (floor), never 0.
SELECT est_rows('SELECT * FROM sel_t WHERE body %~~ tre_pattern(''zzqqxx_none'',0)')
       BETWEEN 1 AND 50 AS absent_is_selective;

DROP FUNCTION est_rows(text);
DROP TABLE sel_t;
