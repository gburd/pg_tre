--
-- planner_auto.sql - Comprehensive planner cost estimation tests
--
-- Demonstrates that pg_tre's cost model produces realistic estimates
-- and the planner makes correct choices based on selectivity and table size.
--

CREATE EXTENSION IF NOT EXISTS pg_tre;

-- Test fixture: small table
CREATE TABLE planner_auto_small (id int, body text);
INSERT INTO planner_auto_small
    SELECT i, 'row_' || i || ' test pattern selective unique content'
    FROM generate_series(1, 100) AS i;

-- Add rows with specific patterns for selectivity tests
INSERT INTO planner_auto_small VALUES
    (101, 'approximate match pattern here'),
    (102, 'another approximate example'),
    (103, 'test case for selectivity verification');

CREATE INDEX idx_auto_small ON planner_auto_small USING tre (body);
ANALYZE planner_auto_small;

\echo '=== Small table (103 rows) ==='
\echo '--- Test 1: Selective pattern (2/103 rows) ---'
-- For small tables, seq scan is often cheaper even for selective patterns.
-- This is correct planner behavior: reading 2 pages vs. index lookup + heap fetch.
EXPLAIN (COSTS ON)
    SELECT * FROM planner_auto_small WHERE body %~~ tre_pattern('approximate', 0);

\echo '--- Test 2: Very selective pattern (1/103 rows) ---'
EXPLAIN (COSTS ON)
    SELECT * FROM planner_auto_small WHERE body %~~ tre_pattern('row_42', 0);

\echo '--- Test 3: Always-true pattern (single char "t") ---'
-- Should return disable_cost to force seq scan
EXPLAIN (COSTS ON)
    SELECT * FROM planner_auto_small WHERE body %~~ tre_pattern('t', 0);

\echo '--- Test 4: Pattern with k=1 (approximate) ---'
EXPLAIN (COSTS ON)
    SELECT * FROM planner_auto_small WHERE body %~~ tre_pattern('approximate', 1);

\echo ''
\echo '=== Force index scan to compare costs ==='
\echo '--- Selective pattern with enable_seqscan=off ---'
SET enable_seqscan = off;
EXPLAIN (COSTS ON)
    SELECT * FROM planner_auto_small WHERE body %~~ tre_pattern('approximate', 0);

\echo '--- Always-true pattern with enable_seqscan=off ---'
-- Should still have very high cost (disable_cost)
EXPLAIN (COSTS ON)
    SELECT * FROM planner_auto_small WHERE body %~~ tre_pattern('t', 0);

\echo '--- k=2 pattern with enable_seqscan=off ---'
EXPLAIN (COSTS ON)
    SELECT * FROM planner_auto_small WHERE body %~~ tre_pattern('test', 2);

SET enable_seqscan = on;

\echo ''
\echo '=== Medium table (500 rows) ==='
-- Test with a slightly larger table (still below multi-leaf posting threshold)
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

\echo '--- Test 5: Selective pattern on 303-row table ---'
EXPLAIN (COSTS ON)
    SELECT * FROM planner_auto_medium WHERE body %~~ tre_pattern('findme', 0);

\echo '--- Test 6: Non-selective pattern ---'
EXPLAIN (COSTS ON)
    SELECT * FROM planner_auto_medium WHERE body %~~ tre_pattern('e', 0);

\echo ''
\echo '=== Verify correctness (sanity check) ==='
SELECT COUNT(*) AS approximate_matches 
FROM planner_auto_small 
WHERE body %~~ tre_pattern('approximate', 0);

SELECT COUNT(*) AS row_42_matches 
FROM planner_auto_small 
WHERE body %~~ tre_pattern('row_42', 0);

SELECT COUNT(*) AS findme_matches 
FROM planner_auto_medium 
WHERE body %~~ tre_pattern('findme', 0);

-- Cleanup
DROP TABLE planner_auto_small;
DROP TABLE planner_auto_medium;
