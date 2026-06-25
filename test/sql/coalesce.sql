-- Posting-page coalescing (v2.0, format v9).
--
-- Coalescing packs medium-cardinality trigram postings onto shared
-- pages instead of one dedicated page each.  It changes ON-DISK SIZE
-- only -- never what the index matches (recheck stays authoritative).
-- This test asserts, in the robust single-token style:
--   1. A coalesce-ON build returns the SAME rows as a seq-scan
--      (correctness preserved across the new write+read path).
--   2. The coalesce-ON index is NO LARGER than the coalesce-OFF index
--      (the size win is best-effort and corpus-dependent, but the
--      feature must never regress size).
--   3. Both indexes are at on-disk format v9 (the additive bump landed;
--      no page is below v8 in a from-scratch build).
--
-- We do NOT assert exact trigram/page counts or that coalescing
-- definitely engaged on this fixture (that is fragile); CI exercises
-- the engaged path on the full corpus.
CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS coalesce_docs;
CREATE TABLE coalesce_docs (id serial PRIMARY KEY, body text);

-- Build a corpus where some trigrams recur in many scattered rows
-- (large serialized sparsemaps that exceed the inline threshold) and
-- others are rare.  ~2000 rows, a shared token plus per-row variation.
-- The 'midtoken' below appears in a large, scattered subset so its
-- serialized sparsemap is sizeable -- the medium-bucket case coalescing
-- targets.  (If it still lands inline or out-of-line on this fixture
-- the test stays correct; CI exercises the engaged path on the full
-- corpus.)
INSERT INTO coalesce_docs(body)
SELECT 'commonword item' || g::text ||
       (CASE WHEN g %  7 = 0 THEN ' alpha'     ELSE '' END) ||
       (CASE WHEN g % 11 = 0 THEN ' bravo'     ELSE '' END) ||
       (CASE WHEN (g * 2654435761) % 3 = 0 THEN ' midtoken' ELSE '' END) ||
       (CASE WHEN g % 101 = 0 THEN ' raretoken' ELSE '' END)
FROM generate_series(1, 2000) g;
ANALYZE coalesce_docs;

SET client_min_messages = 'warning';

-- Baseline index (coalescing off).
SET pg_tre.coalesce_enable = off;
CREATE INDEX coalesce_off_idx ON coalesce_docs USING tre (body);

-- Coalesced index (coalescing on).
SET pg_tre.coalesce_enable = on;
CREATE INDEX coalesce_on_idx ON coalesce_docs USING tre (body);
SET pg_tre.coalesce_enable = off;

RESET client_min_messages;

-- (1) Correctness: coalesce-ON index results == seq-scan results, for a
-- common token, a periodic token, and a rare token.  We drop the OFF
-- index first so the planner uses the ON index for the index path.
DROP INDEX coalesce_off_idx;

CREATE OR REPLACE FUNCTION coalesce_diff(pat text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
  seq_n bigint;
  idx_n bigint;
BEGIN
  SET LOCAL enable_indexscan = off;
  SET LOCAL enable_bitmapscan = off;
  SET LOCAL enable_seqscan = on;
  EXECUTE format('SELECT count(*) FROM coalesce_docs '
                 'WHERE tre_amatch(body, %L, 0)', pat) INTO seq_n;

  SET LOCAL enable_indexscan = on;
  SET LOCAL enable_bitmapscan = on;
  SET LOCAL enable_seqscan = off;
  EXECUTE format('SELECT count(*) FROM coalesce_docs '
                 'WHERE body %%~~ tre_pattern(%L, 0)', pat) INTO idx_n;

  IF seq_n = idx_n THEN RETURN 'ok'; ELSE RETURN 'bad'; END IF;
END $$;

SELECT coalesce_diff('commonword') AS common_result;
SELECT coalesce_diff('alpha')      AS alpha_result;
SELECT coalesce_diff('midtoken')   AS mid_result;
SELECT coalesce_diff('raretoken')  AS rare_result;

-- (2) Size non-regression: rebuild the OFF index and compare sizes.
SET client_min_messages = 'warning';
SET pg_tre.coalesce_enable = off;
CREATE INDEX coalesce_off_idx ON coalesce_docs USING tre (body);
RESET client_min_messages;

SELECT CASE WHEN pg_relation_size('coalesce_on_idx')
              <= pg_relation_size('coalesce_off_idx')
            THEN 'ok' ELSE 'bad' END AS size_result;

-- (3) Format v8: every page of the coalesced index is at format v9
-- (no page is below the latest version in a from-scratch build).
SELECT CASE WHEN min(format_version) = 9 THEN 'ok' ELSE 'bad' END
         AS format_result
FROM pg_tre_index_format_status('coalesce_on_idx'::regclass);

DROP FUNCTION coalesce_diff(text);
DROP TABLE coalesce_docs;
