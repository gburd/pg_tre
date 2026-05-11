--
-- planner.sql - Phase 6 planner cost estimation tests
--
-- Verify that the planner chooses index scan for selective patterns
-- and seq scan for non-selective patterns, based on real cost estimates.
--

-- Test fixture: small table to keep tests fast
CREATE TABLE planner_test (id int, body text);
INSERT INTO planner_test
    SELECT i, 'row_' || i || ' test pattern selective unique'
    FROM generate_series(1, 100) AS i;

-- Add some rows with specific patterns
INSERT INTO planner_test VALUES
    (101, 'approximate match pattern here'),
    (102, 'another approximate example'),
    (103, 'test case for selectivity');

-- Create index
CREATE INDEX idx_planner_test ON planner_test USING tre (body);

-- Force ANALYZE to update statistics
ANALYZE planner_test;

-- Wait a moment for stats to be available
SELECT pg_sleep(0.1);

-- Test 1: Selective pattern should use index scan
-- Pattern 'approximate' appears in only 2/103 rows (~2%)
EXPLAIN (COSTS OFF)
    SELECT * FROM planner_test WHERE body %~~ tre_pattern('approximate', 0);

-- Test 2: Very selective pattern should definitely use index scan
EXPLAIN (COSTS OFF)
    SELECT * FROM planner_test WHERE body %~~ tre_pattern('row_42', 0);

-- Test 3: Non-selective pattern (appears in many rows) may use seq scan
-- Single letter 'e' appears in almost every row
EXPLAIN (COSTS OFF)
    SELECT * FROM planner_test WHERE body %~~ tre_pattern('e', 0);

-- Test 4: Pattern with k>0 (approximate match) should still cost appropriately
EXPLAIN (COSTS OFF)
    SELECT * FROM planner_test WHERE body %~~ tre_pattern('approximate', 1);

-- Test 5: Verify actual results are correct (sanity check)
SELECT COUNT(*) FROM planner_test WHERE body %~~ tre_pattern('approximate', 0);
SELECT COUNT(*) FROM planner_test WHERE body %~~ tre_pattern('row_42', 0);

-- Cleanup
DROP TABLE planner_test;
