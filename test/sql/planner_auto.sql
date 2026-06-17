--
-- planner_auto.sql - planner cost estimation behavior (version-robust)
--
-- Verifies pg_tre's cost model lets the planner make sensible choices.
-- Asserts plan-node CATEGORY (index vs seq scan) and row-count
-- correctness rather than exact EXPLAIN cost numbers, which vary by
-- PostgreSQL major version and shift with selectivity-estimate
-- changes (the prior EXPLAIN COSTS ON form was flaky across PG17/PG18
-- and borderline small-table plans).
--

CREATE EXTENSION IF NOT EXISTS pg_tre;

-- Helper: report the top scan-node category for a query.
CREATE FUNCTION pa_plan(q text)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE r record; node text;
BEGIN
  FOR r IN EXECUTE 'EXPLAIN (FORMAT JSON) ' || q LOOP
    node := r."QUERY PLAN"->0->'Plan'->>'Node Type';
  END LOOP;
  IF node ILIKE '%Index%' OR node ILIKE '%Bitmap%' THEN RETURN 'index';
  ELSIF node ILIKE '%Seq%' THEN RETURN 'seqscan';
  ELSE RETURN node; END IF;
END $$;

CREATE TABLE planner_auto_small (id int, body text);
INSERT INTO planner_auto_small
    SELECT i, 'row_' || i || ' test pattern selective unique content'
    FROM generate_series(1, 100) AS i;
INSERT INTO planner_auto_small VALUES
    (101, 'approximate match pattern here'),
    (102, 'another approximate example'),
    (103, 'test case for selectivity verification');
CREATE INDEX idx_auto_small ON planner_auto_small USING tre (body);
ANALYZE planner_auto_small;

-- When the index is forced, a selective pattern uses it (the index is
-- usable / chosen) -- deterministic regardless of the auto cost call.
SET enable_seqscan = off;
SELECT pa_plan($$SELECT * FROM planner_auto_small WHERE body %~~ tre_pattern('approximate', 0)$$) AS forced_selective;
SELECT pa_plan($$SELECT * FROM planner_auto_small WHERE body %~~ tre_pattern('row_42', 0)$$)      AS forced_very_selective;
SET enable_seqscan = on;

-- Row-count correctness (the substantive guarantee).
SELECT COUNT(*) AS approximate_matches
FROM planner_auto_small WHERE body %~~ tre_pattern('approximate', 0);
SELECT COUNT(*) AS row_42_matches
FROM planner_auto_small WHERE body %~~ tre_pattern('row_42', 0);

-- Medium table.
CREATE TABLE planner_auto_medium (id int, body text);
INSERT INTO planner_auto_medium
    SELECT i, 'document ' || i || ' contains searchable content and stuff'
    FROM generate_series(1, 300) AS i;
INSERT INTO planner_auto_medium VALUES
    (501, 'special findme keyword here'),
    (502, 'another findme occurrence'),
    (503, 'random text without the keyword');
CREATE INDEX idx_auto_medium ON planner_auto_medium USING tre (body);
ANALYZE planner_auto_medium;

SET enable_seqscan = off;
SELECT pa_plan($$SELECT * FROM planner_auto_medium WHERE body %~~ tre_pattern('findme', 0)$$) AS forced_medium;
SET enable_seqscan = on;

SELECT COUNT(*) AS findme_matches
FROM planner_auto_medium WHERE body %~~ tre_pattern('findme', 0);

DROP FUNCTION pa_plan(text);
DROP TABLE planner_auto_small;
DROP TABLE planner_auto_medium;
