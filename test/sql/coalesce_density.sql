-- Posting-page coalescing DENSITY win (Blocker 1, format v9).
--
-- v2.0 shipped coalescing with a narrow medium bucket (2048, 3072]: a
-- coalesced page only ever packed ~2 slots, and the entire
-- (3072, single-leaf-budget] band -- the bulk of medium/high-cardinality
-- trigrams on real corpora -- still took one dedicated 8 KB leaf each.
-- That was the ~60.9 KB/row density blocker.
--
-- This test exercises the WIDENED band: PG_TRE_COALESCE_MAX is now the
-- ">=2 slots fit on a page" boundary (~4056 bytes), so postings in
-- (2048, ~4056] share coalesced pages 2-3 to a page instead of one
-- dedicated leaf each.  It asserts, in the robust single-token style:
--   1. coalesce-ON results == seq-scan results (recheck authoritative;
--      layout-only change, never what the index matches).
--   2. Coalescing ENGAGED on this fixture: at least one coalesced page
--      exists (so the wider band actually packed medium postings).
--   3. The coalesce-ON index is STRICTLY smaller than the coalesce-OFF
--      index (the density win, not merely non-regression).
--   4. The ON index is at on-disk format v9 (additive; no REINDEX).
--
-- The exact page/byte counts are not hard-coded (fragile across
-- sparsemap revisions); we assert the direction of the win only.
SET client_min_messages = 'warning';

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS density_docs;
CREATE TABLE density_docs (id serial PRIMARY KEY, body text);

-- Corpus tuned so several distinct tokens each occur at a moderate,
-- scattered cadence.  A token spread across ~1500-5000 scattered TIDs
-- serializes to a multi-KB sparsemap that lands in the widened medium
-- band (2048, ~4056]; the coalesced writer packs several such blobs
-- onto shared pages.  The 'filler'||g prefix also contributes many
-- band-sized digit trigrams.  20000 rows keeps the fixture quick while
-- giving the band enough material to engage and save pages.
INSERT INTO density_docs(body)
SELECT 'filler' || g::text
       || (CASE WHEN g %  4 = 0 THEN ' qzxa' ELSE '' END)
       || (CASE WHEN g %  5 = 0 THEN ' qzxb' ELSE '' END)
       || (CASE WHEN g %  7 = 0 THEN ' qzxc' ELSE '' END)
       || (CASE WHEN g % 10 = 0 THEN ' qzxd' ELSE '' END)
       || (CASE WHEN g % 13 = 0 THEN ' qzxe' ELSE '' END)
       || (CASE WHEN g % 211 = 0 THEN ' raretoken' ELSE '' END)
FROM generate_series(1, 20000) g;
ANALYZE density_docs;

-- Baseline: coalescing off (one dedicated leaf per medium trigram).
SET pg_tre.coalesce_enable = off;
CREATE INDEX density_off_idx ON density_docs USING tre (body);

-- Widened-band coalescing on.
SET pg_tre.coalesce_enable = on;
CREATE INDEX density_on_idx ON density_docs USING tre (body);
SET pg_tre.coalesce_enable = off;

-- (1) Correctness: ON index == seq-scan for a common, a periodic, and a
-- rare token.  Drop the OFF index first so the planner uses the ON one.
DROP INDEX density_off_idx;

CREATE OR REPLACE FUNCTION density_diff(pat text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
  seq_n bigint;
  idx_n bigint;
BEGIN
  SET LOCAL enable_indexscan = off;
  SET LOCAL enable_bitmapscan = off;
  SET LOCAL enable_seqscan = on;
  EXECUTE format('SELECT count(*) FROM density_docs '
                 'WHERE tre_amatch(body, %L, 0)', pat) INTO seq_n;

  SET LOCAL enable_indexscan = on;
  SET LOCAL enable_bitmapscan = on;
  SET LOCAL enable_seqscan = off;
  EXECUTE format('SELECT count(*) FROM density_docs '
                 'WHERE body %%~~ tre_pattern(%L, 0)', pat) INTO idx_n;

  IF seq_n = idx_n THEN RETURN 'ok'; ELSE RETURN 'bad'; END IF;
END $$;

SELECT density_diff('qzxa')      AS qzxa_result;
SELECT density_diff('qzxc')      AS qzxc_result;
SELECT density_diff('raretoken') AS rare_result;

-- (2) Coalescing engaged: the widened band packed at least one
-- coalesced page (the VACUUM/merge safety paths are thereby exercised).
SELECT CASE WHEN tre_coalesced_page_count('density_on_idx'::regclass) > 0
            THEN 'ok' ELSE 'bad' END AS engaged_result;

-- (3) Density win: rebuild the OFF index and assert the ON index is
-- STRICTLY smaller (the wider band collapses dedicated leaves into
-- shared coalesced pages).
SET client_min_messages = 'warning';
SET pg_tre.coalesce_enable = off;
CREATE INDEX density_off_idx ON density_docs USING tre (body);

SELECT CASE WHEN pg_relation_size('density_on_idx')
              < pg_relation_size('density_off_idx')
            THEN 'ok' ELSE 'bad' END AS density_result;

-- (4) Format v8: every page of the ON index is at format v9.
SELECT CASE WHEN min(format_version) = 9 THEN 'ok' ELSE 'bad' END
         AS format_result
FROM pg_tre_index_format_status('density_on_idx'::regclass);

DROP FUNCTION density_diff(text);
DROP TABLE density_docs;
