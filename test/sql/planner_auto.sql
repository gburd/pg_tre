--
-- planner_auto.sql - planner cost estimation behavior (version-robust)
--
-- Asserts plan-node category and row-count correctness via single
-- text tokens (no EXPLAIN cost dumps, no multi-column tables, NOTICEs
-- suppressed) so the expected file is stable across PG major versions
-- and selectivity-estimate changes.
--
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_tre;

CREATE FUNCTION pa_uses_index(q text)
RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE r record; node text;
BEGIN
  FOR r IN EXECUTE 'EXPLAIN (FORMAT JSON) ' || q LOOP
    node := r."QUERY PLAN"->0->'Plan'->>'Node Type';
  END LOOP;
  RETURN node ILIKE '%Index%' OR node ILIKE '%Bitmap%';
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

-- Forced index scan is usable for selective patterns; results correct.
SET enable_seqscan = off;
SELECT CASE WHEN pa_uses_index($$SELECT * FROM planner_auto_small WHERE body %~~ tre_pattern('approximate', 0)$$)
            AND (SELECT count(*) FROM planner_auto_small WHERE body %~~ tre_pattern('approximate', 0)) = 2
            THEN 'small_selective_ok' ELSE 'small_selective_bad' END AS r1;
SELECT CASE WHEN pa_uses_index($$SELECT * FROM planner_auto_small WHERE body %~~ tre_pattern('row_42', 0)$$)
            AND (SELECT count(*) FROM planner_auto_small WHERE body %~~ tre_pattern('row_42', 0)) = 1
            THEN 'small_very_selective_ok' ELSE 'small_very_selective_bad' END AS r2;
SET enable_seqscan = on;

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
SELECT CASE WHEN pa_uses_index($$SELECT * FROM planner_auto_medium WHERE body %~~ tre_pattern('findme', 0)$$)
            AND (SELECT count(*) FROM planner_auto_medium WHERE body %~~ tre_pattern('findme', 0)) = 2
            THEN 'medium_ok' ELSE 'medium_bad' END AS r3;
SET enable_seqscan = on;

DROP FUNCTION pa_uses_index(text);
DROP TABLE planner_auto_small;
DROP TABLE planner_auto_medium;
