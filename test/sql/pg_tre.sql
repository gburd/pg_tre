-- Regression test for pg_tre extension.

CREATE EXTENSION pg_tre;

-- The access method is registered.
SELECT amname, amtype FROM pg_am WHERE amname = 'tre';

-- Legacy UDFs: basic exact match (cost 0)
SELECT tre_amatch('hello', 'hello', 0);

-- Approximate match: 'colour' matches regex 'color' within distance 1
SELECT tre_amatch('colour', 'color', 1);

-- Regex approximate match: enviro.*ment matches 'environment' exactly
SELECT tre_amatch('environment', 'enviro.*ment', 2);

-- Cost function returns the actual edit cost
SELECT tre_amatch_cost('colour', 'color', 3);

-- No match within threshold
SELECT tre_amatch('abcdef', 'xyz', 1);

-- Custom per-operation costs
SELECT tre_amatch('colour', 'color', 2, 1, 1, 1);

-- Detail function shows cost breakdown
SELECT * FROM tre_amatch_detail('colour', 'color', 3);

-- Version string
SELECT tre_version();

-- Edge cases: empty strings
SELECT tre_amatch('', '', 0);
SELECT tre_amatch('a', '', 1);

-- NULL handling (STRICT functions return NULL on NULL input)
SELECT tre_amatch(NULL, 'test', 1);
SELECT tre_amatch('test', NULL, 1);

-- CREATE INDEX succeeds syntactically (Phase 0 skeleton):
-- the actual build is a stub and raises a NOTICE.
CREATE TABLE tre_test (t text);
INSERT INTO tre_test VALUES ('hello'), ('world');
SET client_min_messages = 'notice';
CREATE INDEX tre_test_idx ON tre_test USING tre (t);
RESET client_min_messages;
DROP INDEX tre_test_idx;
DROP TABLE tre_test;
