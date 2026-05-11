-- Phase 4: pending list + aminsert + VACUUM merge.
--
-- Fixture: create index on initial rows (go to posting trees),
-- insert more rows (go to pending list), verify the scan sees
-- both groups.  Then VACUUM to drain pending list; verify the
-- scan still returns the same rows.

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS inc;
CREATE TABLE inc (id serial PRIMARY KEY, body text);
INSERT INTO inc(body) VALUES ('apple pie'), ('cherry tart');

SET client_min_messages = 'warning';
CREATE INDEX inc_idx ON inc USING tre (body);
RESET client_min_messages;

-- Rows after the index is built go to the pending list.
INSERT INTO inc(body) VALUES
  ('apple fritter'),
  ('banana split'),
  ('apple cobbler'),
  ('grape jelly');

-- Differential: every pattern's index-scan row set equals seq-scan.
CREATE OR REPLACE FUNCTION inc_diff(pat text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE seq_ids int[]; idx_ids int[];
BEGIN
  SET LOCAL enable_indexscan=off;
  SET LOCAL enable_bitmapscan=off;
  SET LOCAL enable_seqscan=on;
  EXECUTE format('SELECT array_agg(id ORDER BY id) FROM inc '
                 'WHERE tre_amatch(body, %L, 0)', pat) INTO seq_ids;
  SET LOCAL enable_seqscan=off;
  SET LOCAL enable_indexscan=on;
  SET LOCAL enable_bitmapscan=on;
  EXECUTE format('SELECT array_agg(id ORDER BY id) FROM inc '
                 'WHERE body %%~~ tre_pattern(%L, 0)', pat) INTO idx_ids;
  IF seq_ids IS NOT DISTINCT FROM idx_ids THEN
    RETURN 'OK  ' || pat;
  END IF;
  RETURN format('BAD %s  seq=%s  idx=%s', pat, seq_ids, idx_ids);
END$$;

-- Pre-VACUUM: pending list is populated with rows 3..6.
SELECT inc_diff('apple');    -- must find 1, 3, 5
SELECT inc_diff('banana');   -- must find 4 (pending only)
SELECT inc_diff('grape');    -- must find 6 (pending only)
SELECT inc_diff('cherry');   -- must find 2 (posting only)
SELECT inc_diff('xyz');      -- no match

-- Drain the pending list.
VACUUM inc;

-- Post-VACUUM: all rows in posting trees.  Results must be identical.
SELECT inc_diff('apple');
SELECT inc_diff('banana');
SELECT inc_diff('grape');
SELECT inc_diff('cherry');
SELECT inc_diff('xyz');

DROP FUNCTION inc_diff(text);
DROP INDEX inc_idx;
DROP TABLE inc;
