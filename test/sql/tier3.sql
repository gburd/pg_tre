-- Test tier-3 per-tuple bloom filter functionality
-- Phase 5: Verify that tier-3 filtering correctly refines candidate TIDs

-- Setup: create a table with known patterns
CREATE TABLE tier3_test (id serial PRIMARY KEY, body text);

-- Insert rows: some with 'fox', some without
INSERT INTO tier3_test(body) VALUES
  ('the quick brown fox'),
  ('lazy dog jumps over'),
  ('foxes are quick animals'),
  ('another fox appears'),
  ('no match here xyz'),
  ('fox fox fox'),
  ('quick and dirty fox'),
  ('the red fox runs fast'),
  ('completely different text'),
  ('more random content');

-- Create index with tier-3 enabled (default)
CREATE INDEX tier3_test_idx ON tier3_test USING tre (body);

-- Verify tier-3 is working: exact match for 'fox'
SET enable_seqscan = off;

SELECT id, body FROM tier3_test 
WHERE body %~~ tre_pattern('fox', 0) 
ORDER BY id;

-- Expected: 6 rows (ids 1, 3, 4, 6, 7, 8)

-- Verify with seq scan for correctness check
SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;

SELECT id, body FROM tier3_test 
WHERE body %~~ tre_pattern('fox', 0) 
ORDER BY id;

-- Should match index scan results

-- Test approximate match (tier-3 should work with k>0 too)
SET enable_seqscan = off;
SET enable_bitmapscan = on;

SELECT id, body FROM tier3_test 
WHERE body %~~ tre_pattern('fox', 1) 
ORDER BY id;

-- Expected: same 6 rows plus possibly 'foxes' variants

-- Cleanup
DROP TABLE tier3_test CASCADE;
