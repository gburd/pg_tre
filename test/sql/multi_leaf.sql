-- test/sql/multi_leaf.sql
-- Regression test for multi-leaf posting trees (Phase 4.2).
--
-- Objective: Force a trigram to exceed the single-leaf posting budget
-- (~8 KB) so the build splits it across multiple leaves linked by
-- right_link.  Verify that index scans return the same rows as seq-scans.

-- Load the extension (requires preload).
CREATE EXTENSION IF NOT EXISTS pg_tre;

-- Setup: generate 100K rows where the trigram 'the' appears in every row.
-- At ~8 bytes per TID after sparsemap compression, 100K TIDs = ~800 KB,
-- far exceeding the ~8 KB single-leaf budget.
DROP TABLE IF EXISTS multi_leaf_test CASCADE;
CREATE TABLE multi_leaf_test (id serial PRIMARY KEY, body text);

-- Insert 100K rows with 'the' trigram present in every row.
INSERT INTO multi_leaf_test (body)
SELECT 'The quick brown fox jumps over the lazy dog. Row ' || i
FROM generate_series(1, 100000) AS i;

-- Create pg_tre index (should split 'the' posting across multiple leaves).
CREATE INDEX multi_leaf_idx ON multi_leaf_test USING tre (body);

-- Verify index built successfully (no errcode_program_limit_exceeded).
SELECT pg_relation_size('multi_leaf_idx') > 0 AS index_exists;

-- Differential test 1: exact match for 'the' (should return all 100K rows).
-- Compare index scan vs seq-scan.
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT COUNT(*) FROM multi_leaf_test WHERE body %~~ tre_pattern('the', 0);
SELECT COUNT(*) FROM multi_leaf_test WHERE body %~~ tre_pattern('the', 0);

SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF)
SELECT COUNT(*) FROM multi_leaf_test WHERE body %~~ tre_pattern('the', 0);
SELECT COUNT(*) FROM multi_leaf_test WHERE body %~~ tre_pattern('the', 0);

SET enable_indexscan = on;
SET enable_bitmapscan = on;

-- Differential test 2: pattern that matches a subset of rows.
SET enable_seqscan = off;
SELECT COUNT(*) FROM multi_leaf_test
WHERE body %~~ tre_pattern('Row 12[0-9][0-9][0-9]', 0);

SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT COUNT(*) FROM multi_leaf_test
WHERE body %~~ tre_pattern('Row 12[0-9][0-9][0-9]', 0);

SET enable_indexscan = on;
SET enable_bitmapscan = on;

-- Differential test 3: approximate match (k=1).
SET enable_seqscan = off;
SELECT COUNT(*) FROM multi_leaf_test
WHERE body %~~ tre_pattern('quik', 1);  -- should match 'quick'

SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT COUNT(*) FROM multi_leaf_test
WHERE body %~~ tre_pattern('quik', 1);

SET enable_indexscan = on;
SET enable_bitmapscan = on;

-- Differential test 4: non-matching pattern (should return 0 rows).
SET enable_seqscan = off;
SELECT COUNT(*) FROM multi_leaf_test
WHERE body %~~ tre_pattern('xyzabc', 0);

SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT COUNT(*) FROM multi_leaf_test
WHERE body %~~ tre_pattern('xyzabc', 0);

-- Reset planner settings.
SET enable_indexscan = on;
SET enable_bitmapscan = on;
SET enable_seqscan = on;

-- REINDEX test: drop and rebuild the index to verify multi-leaf rebuild.
REINDEX INDEX multi_leaf_idx;

-- Verify rebuilt index still works correctly.
SET enable_seqscan = off;
SELECT COUNT(*) FROM multi_leaf_test WHERE body %~~ tre_pattern('the', 0);
SET enable_seqscan = on;

-- Cleanup.
DROP INDEX multi_leaf_idx;
DROP TABLE multi_leaf_test;
