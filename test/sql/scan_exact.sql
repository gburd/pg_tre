-- Phase 3 scan test: exact regex via the index, differential against seq-scan.
--
-- For each pattern, compare the row-id set returned by:
--   (a) SEQ scan with tre_amatch(body, pat, 0)
--   (b) INDEX scan with body %~~ tre_pattern(pat, 0)
-- They must match exactly.

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS scan_exact_docs;
CREATE TABLE scan_exact_docs (id serial PRIMARY KEY, body text);
INSERT INTO scan_exact_docs(body) VALUES
  ('hello world'),
  ('hello there, friend'),
  ('goodbye world'),
  ('colour television'),
  ('colour is color'),
  ('approximate regex matching'),
  ('the quick brown fox'),
  ('jumped over the lazy dog'),
  ('PostgreSQL is fast'),
  ('pg_tre indexes regexes');

SET client_min_messages = 'warning';
CREATE INDEX scan_exact_idx ON scan_exact_docs USING tre (body);
RESET client_min_messages;

-- Differential helper: returns 'OK ...' or 'BAD ...' for one pattern.
CREATE OR REPLACE FUNCTION scan_exact_diff(pat text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
  seq_ids int[];
  idx_ids int[];
BEGIN
  SET LOCAL enable_indexscan=off;
  SET LOCAL enable_bitmapscan=off;
  SET LOCAL enable_seqscan=on;
  EXECUTE format('SELECT array_agg(id ORDER BY id) FROM scan_exact_docs '
                 'WHERE tre_amatch(body, %L, 0)', pat)
    INTO seq_ids;

  SET LOCAL enable_seqscan=off;
  SET LOCAL enable_indexscan=on;
  SET LOCAL enable_bitmapscan=on;
  EXECUTE format('SELECT array_agg(id ORDER BY id) FROM scan_exact_docs '
                 'WHERE body %%~~ tre_pattern(%L, 0)', pat)
    INTO idx_ids;

  IF seq_ids IS NOT DISTINCT FROM idx_ids THEN
    RETURN 'OK  ' || pat;
  END IF;
  RETURN format('BAD %s  seq=%s  idx=%s', pat, seq_ids, idx_ids);
END$$;

SELECT scan_exact_diff('hello');
SELECT scan_exact_diff('world');
SELECT scan_exact_diff('color');
SELECT scan_exact_diff('the');
SELECT scan_exact_diff('quick');
SELECT scan_exact_diff('fox');
SELECT scan_exact_diff('regex');
SELECT scan_exact_diff('television');
SELECT scan_exact_diff('approximate');
SELECT scan_exact_diff('PostgreSQL');
SELECT scan_exact_diff('pg_tre');
SELECT scan_exact_diff('zebra');                        -- no match
SELECT scan_exact_diff('xyz');                          -- no match

-- Verify EXPLAIN picks the bitmap index scan.
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT id FROM scan_exact_docs WHERE body %~~ tre_pattern('hello', 0);

DROP FUNCTION scan_exact_diff(text);
DROP INDEX scan_exact_idx;
DROP TABLE scan_exact_docs;
