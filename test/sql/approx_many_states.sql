-- test/sql/approx_many_states.sql
--
-- Regression test for the approximate matcher's delete-handling deque
-- (patches/tre-approx-deque-grow.patch; upstream laurikari/tre #156,
-- issue #154).  The deque was a fixed 512-pointer stack array with no
-- bound check on the initial fill, so a pattern whose NFA has more than
-- 512 states reachable at one position overflowed it and crashed the
-- backend.  tre_amatch() / %~~ at edit distance >= 1 reach it, so any
-- unprivileged user could crash the server.
--
-- These queries built a >512-state NFA and segfaulted on 4.5.2 and
-- earlier; they must now return a value.  Edit distance >= 1 selects the
-- approximate matcher (k = 0 uses the parallel matcher, a different path).

CREATE EXTENSION IF NOT EXISTS pg_tre;
\set VERBOSITY terse
SET client_min_messages = warning;

-- A 600-way alternation of 'a': ~600 states live at one position, over
-- the old 512 cap.  'aaaa' is within 1 edit of 'a' repeated, so true.
SELECT tre_amatch('aaaa', (SELECT string_agg('a', '|') FROM generate_series(1, 600)), 1) AS alt600_k1;

-- 2000-way, distance 2.
SELECT tre_amatch('aaaaaaaa', (SELECT string_agg('a', '|') FROM generate_series(1, 2000)), 2) AS alt2000_k2;

-- A large bracket-free run that compiles to many states, matched fuzzily.
SELECT tre_amatch(repeat('ab', 20), (SELECT string_agg('ab', '|') FROM generate_series(1, 1000)), 1) AS alt1000ab_k1;

-- Through the operator and an index recheck: a plain role (the opclass
-- functions are PUBLIC) must not be able to crash the backend.
CREATE TABLE ams (id int, s text);
INSERT INTO ams SELECT g, repeat('a', 1 + g % 6) FROM generate_series(1, 200) g;
CREATE INDEX ams_tre ON ams USING tre (s);
SELECT count(*) AS op_rows
FROM ams
WHERE s %~~ tre_pattern((SELECT string_agg('a', '|') FROM generate_series(1, 600)), 1);

-- Index must agree with a sequential scan for this pattern.
SELECT
  (SELECT count(*) FROM ams WHERE s %~~ tre_pattern((SELECT string_agg('a','|') FROM generate_series(1,600)), 1))
  AS idx_rows
\gset
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT count(*) = :idx_rows AS index_eq_seq
FROM ams WHERE s %~~ tre_pattern((SELECT string_agg('a','|') FROM generate_series(1,600)), 1);
RESET enable_indexscan;
RESET enable_bitmapscan;

SELECT 'survived' AS status;
DROP TABLE ams;
