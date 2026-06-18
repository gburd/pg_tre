-- Phase 5 READ test: approximate regex (k>0) via three-tier filtering
--
-- For each (pattern, k) pair, compare the row-id set returned by:
--   (a) SEQ scan with tre_amatch(body, pat, k)
--   (b) INDEX scan with body %~~ tre_pattern(pat, k)
-- They must match exactly.
--
-- Phase 5 initial cut: tier-3 bloom filtering is active when payload exists;
-- tier-1 range filtering and positional filtering are stubbed (pass-through).

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS p5_docs;
CREATE TABLE p5_docs (id serial PRIMARY KEY, body text);

-- Fixture: 50 rows with varied text for k=1 and k=2 testing
INSERT INTO p5_docs(body) VALUES
  ('hello world'),
  ('helo world'),            -- k=1 from 'hello'
  ('hallo world'),           -- k=1 from 'hello'
  ('helllo world'),          -- k=1 from 'hello'
  ('heelo world'),           -- k=2 from 'hello'
  ('goodbye friend'),
  ('godbye friend'),         -- k=1 from 'goodbye'
  ('goodbey friend'),        -- k=1 from 'goodbye'
  ('colour television'),
  ('color television'),      -- k=1 from 'colour'
  ('coloor television'),     -- k=1 from 'colour'
  ('approximate matching'),
  ('aproximate matching'),   -- k=1 from 'approximate'
  ('approximat matching'),   -- k=1 from 'approximate'
  ('aproxmate matching'),    -- k=2 from 'approximate'
  ('environment variables'),
  ('enviroment variables'),  -- k=1 from 'environment'
  ('envirnment variables'),  -- k=1 from 'environment'
  ('envirment variables'),   -- k=2 from 'environment'
  ('PostgreSQL database'),
  ('PostgrSQL database'),    -- k=1 from 'PostgreSQL'
  ('PostgresQL database'),   -- k=1 from 'PostgreSQL'
  ('PostreSQL database'),    -- k=2 from 'PostgreSQL'
  ('quick brown fox'),
  ('quik brown fox'),        -- k=1 from 'quick'
  ('qwick brown fox'),       -- k=1 from 'quick'
  ('the lazy dog'),
  ('teh lazy dog'),          -- k=1 from 'the'
  ('thhe lazy dog'),         -- k=1 from 'the'
  ('performance tuning'),
  ('performace tuning'),     -- k=1 from 'performance'
  ('performence tuning'),    -- k=1 from 'performance'
  ('perormance tuning'),     -- k=2 from 'performance'
  ('algorithm design'),
  ('algoritm design'),       -- k=1 from 'algorithm'
  ('algorihm design'),       -- k=1 from 'algorithm'
  ('index structure'),
  ('indx structure'),        -- k=1 from 'index'
  ('inex structure'),        -- k=1 from 'index'
  ('btree implementation'),
  ('btre implementation'),   -- k=1 from 'btree'
  ('btree implmentation'),   -- k=1 from 'implementation'
  ('transaction processing'),
  ('transacion processing'), -- k=1 from 'transaction'
  ('query optimization'),
  ('qurey optimization'),    -- k=1 from 'query'
  ('memory management'),
  ('memry management'),      -- k=1 from 'memory'
  ('network protocol'),
  ('netwrk protocol'),       -- k=1 from 'network'
  ('configuration settings'); -- k=0 control

SET client_min_messages = 'warning';
CREATE INDEX p5_idx ON p5_docs USING tre (body);
-- Force VACUUM to drain pending list and build real postings
VACUUM ANALYZE p5_docs;
RESET client_min_messages;

-- Differential helper: returns 'OK ...' or 'BAD ...' for one (pattern, k) pair.
CREATE OR REPLACE FUNCTION p5_diff(pat text, k int) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
  seq_ids int[];
  idx_ids int[];
BEGIN
  SET LOCAL enable_indexscan=off;
  SET LOCAL enable_bitmapscan=off;
  SET LOCAL enable_seqscan=on;
  EXECUTE format('SELECT array_agg(id ORDER BY id) FROM p5_docs '
                 'WHERE tre_amatch(body, %L, %s)', pat, k)
    INTO seq_ids;

  SET LOCAL enable_seqscan=off;
  SET LOCAL enable_indexscan=on;
  SET LOCAL enable_bitmapscan=on;
  EXECUTE format('SELECT array_agg(id ORDER BY id) FROM p5_docs '
                 'WHERE body %%~~ tre_pattern(%L, %s)', pat, k)
    INTO idx_ids;

  IF seq_ids IS NOT DISTINCT FROM idx_ids THEN
    RETURN 'OK  ' || pat || ' (k=' || k || ')';
  END IF;
  RETURN format('BAD %s (k=%s)  seq=%s  idx=%s', pat, k, seq_ids, idx_ids);
END$$;

-- Phase 5 tests: k=1 and k=2 patterns
-- Each pattern should match its exact form plus k-distance variants

-- k=1 tests (single edit: substitution, insertion, deletion)
SELECT p5_diff('hello', 1);
SELECT p5_diff('goodbye', 1);
SELECT p5_diff('colour', 1);
SELECT p5_diff('approximate', 1);
SELECT p5_diff('environment', 1);
SELECT p5_diff('PostgreSQL', 1);
SELECT p5_diff('quick', 1);
SELECT p5_diff('the', 1);      -- 3-char pattern (minimum for trigrams)
SELECT p5_diff('performance', 1);
SELECT p5_diff('algorithm', 1);
SELECT p5_diff('index', 1);
SELECT p5_diff('btree', 1);
SELECT p5_diff('transaction', 1);
SELECT p5_diff('query', 1);
SELECT p5_diff('memory', 1);
SELECT p5_diff('network', 1);

-- k=2 tests (two edits)
SELECT p5_diff('hello', 2);
SELECT p5_diff('approximate', 2);
SELECT p5_diff('environment', 2);
SELECT p5_diff('PostgreSQL', 2);
SELECT p5_diff('performance', 2);

-- Verify the planner can use the index for k>0 patterns.
-- Version-robust: assert plan-node category via single text tokens
-- (no exact plan-shape dump, which differs across PG majors).
CREATE FUNCTION p5_uses_index(q text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE r record; node text;
BEGIN
  FOR r IN EXECUTE 'EXPLAIN (FORMAT JSON) ' || q LOOP
    node := r."QUERY PLAN"->0->'Plan'->>'Node Type';
  END LOOP;
  RETURN node ILIKE '%Index%' OR node ILIKE '%Bitmap%';
END $$;
SET enable_seqscan = off;
SELECT CASE WHEN p5_uses_index($$SELECT id FROM p5_docs WHERE body %~~ tre_pattern('hello', 1)$$)
            THEN 'k1_uses_index' ELSE 'k1_seq' END AS k1_plan;
SELECT CASE WHEN p5_uses_index($$SELECT id FROM p5_docs WHERE body %~~ tre_pattern('approximate', 2)$$)
            THEN 'k2_uses_index' ELSE 'k2_seq' END AS k2_plan;
SET enable_seqscan = on;

DROP FUNCTION p5_uses_index(text);
DROP FUNCTION p5_diff(text, int);
DROP INDEX p5_idx;
DROP TABLE p5_docs;
