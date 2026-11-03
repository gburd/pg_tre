-- test/sql/rep_extract_tight.sql
--
-- A fixed-count repetition must contribute every trigram it forces, so
-- the index stays selective.  Before 4.6.0 the extractor inlined a
-- repetition child at most twice, so 'su{3}' (= 'suuu') extracted only
-- the trigram "suu" and lost "uuu": the index handed ~10x too many
-- candidates to the recheck on large-row corpora (issue #2).  A fixed
-- count is now inlined up to REP_INLINE_MAX copies, so 'su{3}' extracts
-- both "suu" and "uuu".
--
-- This asserts selectivity (how many distinct trigrams the query uses),
-- not just correctness; rep_extract.sql already checks index == seq.

CREATE EXTENSION IF NOT EXISTS pg_tre;
\set VERBOSITY terse
SET client_min_messages = warning;

-- Count the distinct trigrams (conjuncts of one alternative each, plus
-- alternatives) the extractor emits for a k=0 pattern.
CREATE FUNCTION tri_count(pat text) RETURNS int LANGUAGE sql AS $$
  SELECT count(*)::int
  FROM regexp_matches(tre_extract_debug(pat), 'trigram_hash=', 'g');
$$;

-- Fixed repetitions: every forced trigram must appear.
--   su{3}   = s u u u          -> suu, uuu                     (2)
--   ab{3}c  = a b b b c        -> abb, bbb, bbc                (3)
--   x{4}    = x x x x          -> xxx, xxx (dedup) -> xxx      (1)
--   (ab){2} = a b a b          -> aba, bab                     (2)
--   ab{2}c  = a b b c          -> abb, bbc                     (2)
SELECT 'su{3}'   AS pat, tri_count('su{3}')   AS trigrams, 2 AS want
UNION ALL SELECT 'ab{3}c',  tri_count('ab{3}c'),  3
UNION ALL SELECT '(ab){2}', tri_count('(ab){2}'), 2
UNION ALL SELECT 'ab{2}c',  tri_count('ab{2}c'),  2
ORDER BY pat;

-- The headline case, stated as a boolean so a regression is obvious.
SELECT tri_count('su{3}') = 2 AS su3_extracts_both_trigrams;

-- Variable repetitions must stay sound: 'ab+c' must NOT demand "abc"
-- (that would drop "abbc").  It should extract at most the safe windows.
-- Correctness (index == seq) is covered in rep_extract.sql; here we only
-- confirm the fix did not make a variable rep over-demand by checking the
-- row sets still agree.
DROP TABLE IF EXISTS rpt;
CREATE TABLE rpt (id int, s text);
INSERT INTO rpt VALUES (1,'xabbcx'),(2,'zsuuuz'),(3,'suuuu'),(4,'ssuuu'),(5,'su');
INSERT INTO rpt SELECT g, 'filler '||g FROM generate_series(100,400) g;
CREATE INDEX rpt_tre ON rpt USING tre (s);
ANALYZE rpt;
CREATE FUNCTION agree(pat text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a int[]; b int[];
BEGIN
  PERFORM set_config('enable_seqscan','off',true);
  PERFORM set_config('enable_bitmapscan','on',true);
  PERFORM set_config('enable_indexscan','off',true);
  EXECUTE 'SELECT array_agg(id ORDER BY id) FROM rpt WHERE s %~~ tre_pattern($1,0)' INTO a USING pat;
  PERFORM set_config('enable_seqscan','on',true);
  PERFORM set_config('enable_bitmapscan','off',true);
  EXECUTE 'SELECT array_agg(id ORDER BY id) FROM rpt WHERE tre_amatch(s,$1,0)' INTO b USING pat;
  RETURN a IS NOT DISTINCT FROM b;
END $$;
SELECT bool_and(agree(p)) AS all_fixed_and_var_reps_agree
FROM (VALUES ('su{3}'),('su+z'),('ab+c'),('ab{3}c'),('ab{2,}c'),('x{4}'),('(su){3}')) v(p);

DROP TABLE rpt;
DROP FUNCTION agree(text);
DROP FUNCTION tri_count(text);
