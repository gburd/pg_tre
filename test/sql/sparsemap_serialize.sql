-- Test sparsemap serialization under various loads to catch heap corruption
-- This exercises the pending-list merge path that was corrupting memory

CREATE TABLE smap_test (id serial PRIMARY KEY, body text);
CREATE INDEX smap_idx ON smap_test USING tre (body);

-- Insert enough data to trigger pending-list merges
INSERT INTO smap_test (body)
SELECT 'test' || i || ' pattern' || (i % 100)
FROM generate_series(1, 1000) i;

-- Force a merge by inserting more with overlapping trigrams
INSERT INTO smap_test (body)
SELECT 'pattern' || (i % 50) || ' more test'  
FROM generate_series(1, 1000) i;

-- Query to verify no corruption
SELECT count(*) FROM smap_test WHERE body %~~ tre_pattern('test', 0);
SELECT count(*) FROM smap_test WHERE body %~~ tre_pattern('pattern', 0);

-- Cleanup
DROP TABLE smap_test;
