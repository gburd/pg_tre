-- test/sql/rep_extract.sql
--
-- Trigram extraction across a repetition must not demand trigrams a
-- matching row lacks, and EXPLAIN must count index searches.
--
-- Before 4.3.1 a repetition with a minimum of one and room for more ('b+',
-- 'b{1,3}') was inlined as a single copy and the literal run carried on
-- through it, so 'ab+c' required the trigram "abc" and the index silently
-- dropped rows holding only "abbc".  The %~~ operator at k=0 did the same.
-- Now the run ends after a one-character copy that may repeat.
--
-- pg_tre also never counted an index search, so EXPLAIN ANALYZE printed
-- "Index Searches: 0" (PG 18) for scans that had read the index and
-- pg_stat_user_indexes.idx_scan stayed at zero.

CREATE EXTENSION IF NOT EXISTS pg_tre;
SET client_min_messages = warning;

DROP TABLE IF EXISTS rpx;
CREATE TABLE rpx (id int, s text);
INSERT INTO rpx VALUES
  (1, 'xabbcx'), (2, 'xabcx'), (3, 'zsuuuz'), (4, 'qaabccq'),
  (5, 'sususu'), (6, 'abababc'), (7, 'xyzzzzy'), (8, 'abbbbbc');
INSERT INTO rpx SELECT g, 'filler ' || g FROM generate_series(100, 1100) g;
CREATE INDEX rpx_tre ON rpx USING tre (s);
ANALYZE rpx;

CREATE TEMP TABLE pats (p text);
INSERT INTO pats VALUES
  ('ab+c'), ('ab{1,3}c'), ('ab{1,}c'), ('xab+cx'), ('a+bc'), ('abc+'),
  ('su+z'), ('zsu{1,5}z'), ('(ab)+c'), ('(ab){1,3}c'), ('(su){3}'),
  ('su{3}'), ('xyz+y'), ('xyz{2,}y'), ('ab{2}c'), ('ab{2,}c'),
  ('a(b|bb)c'), ('ab*c'), ('ab?c');

-- mode: 'bitmap', 'index' (plain Index Scan) or 'seq'.
CREATE FUNCTION rpx_rows(pat text, mode text, tre bool) RETURNS int[]
LANGUAGE plpgsql AS $$
DECLARE r int[];
BEGIN
  -- Set all three every time: SET LOCAL lasts until the end of the
  -- transaction, so one SELECT calling this repeatedly would otherwise
  -- accumulate settings from earlier calls and compare nothing.
  PERFORM set_config('enable_seqscan',    (mode = 'seq')::text,    true);
  PERFORM set_config('enable_bitmapscan', (mode = 'bitmap')::text, true);
  PERFORM set_config('enable_indexscan',  (mode = 'index')::text,  true);
  IF tre THEN
    EXECUTE 'SELECT array_agg(id) FROM (SELECT id FROM rpx WHERE s %~~ tre_pattern($1, 0)) m'
      INTO r USING pat;
  ELSE
    EXECUTE 'SELECT array_agg(id) FROM (SELECT id FROM rpx WHERE s ~ $1) m'
      INTO r USING pat;
  END IF;
  RETURN (SELECT array_agg(x ORDER BY x) FROM unnest(r) x);
END $$;

SELECT p, rpx_rows(p, 'seq', false) AS via_seq,
       rpx_rows(p, 'bitmap', false) IS NOT DISTINCT FROM rpx_rows(p, 'seq', false) AS bitmap_ok,
       rpx_rows(p, 'index', false) IS NOT DISTINCT FROM rpx_rows(p, 'seq', false) AS index_ok,
       rpx_rows(p, 'bitmap', true) IS NOT DISTINCT FROM rpx_rows(p, 'seq', true) AS tre_bitmap_ok,
       rpx_rows(p, 'index', true) IS NOT DISTINCT FROM rpx_rows(p, 'seq', true) AS tre_index_ok
  FROM pats ORDER BY p;

-- Index searches are counted, once per scan, on both scan paths.
SET enable_seqscan = off;
SET enable_indexscan = off;
CREATE FUNCTION rpx_searches(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE l text; out text := '';
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
    IF l LIKE '%Index Searches%' THEN out := out || btrim(l); END IF;
  END LOOP;
  RETURN out;
END $$;
SELECT current_setting('server_version_num')::int < 180000
       OR rpx_searches($$SELECT * FROM rpx WHERE s ~ 'ab+c'$$) = 'Index Searches: 1'
       AS bitmap_counts_search;
RESET enable_indexscan;
SET enable_bitmapscan = off;
SELECT current_setting('server_version_num')::int < 180000
       OR rpx_searches($$SELECT * FROM rpx WHERE s ~ 'ab+c'$$) = 'Index Searches: 1'
       AS indexscan_counts_search;
RESET enable_bitmapscan;
RESET enable_seqscan;

SELECT pg_stat_force_next_flush();
SELECT idx_scan > 0 AS idx_scan_counted
  FROM pg_stat_user_indexes WHERE indexrelname = 'rpx_tre';

DROP TABLE rpx;
