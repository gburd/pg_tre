-- test/sql/class_dnf.sql
--
-- A single contiguous run with character-class positions is extracted as
-- exact PIECES (DNF): one piece per concrete string, each the AND of its
-- own trigrams, OR-ed across strings.  The default CNF (per-window
-- OR-groups AND-ed) is sound but accepts cross-window combinations no
-- single matching string needs, so it hands the recheck more candidates.
--
-- 'aa[bc]c[b-d]' expands to 6 strings (aabcb aabcc aabcd aaccb aaccc
-- aaccd); CNF gave {aab,aac} AND {abc,acc} AND {6 trigrams}; PIECES gives
-- the OR of 6 three-trigram ANDs (measured on a 400k EXPLAIN-plan corpus:
-- 1026 CNF candidates vs 465 that hold all trigrams of one string).
-- Issue #2.

CREATE EXTENSION IF NOT EXISTS pg_tre;
\set VERBOSITY terse
SET client_min_messages = warning;

-- Classful single runs take PIECES mode with one piece per concrete string.
SELECT split_part(tre_extract_debug('aa[bc]c[b-d]'), E'\n', 1) AS line1;  -- k=0 mode=PIECES
SELECT tre_extract_debug('aa[bc]c[b-d]') LIKE '%PIECES%' AS is_pieces,
       (SELECT count(*) FROM regexp_matches(tre_extract_debug('aa[bc]c[b-d]'),
               'Piece ', 'g')) AS piece_count;

-- A pure-literal run stays CNF (nothing to gain from PIECES).
SELECT tre_extract_debug('abcdef') LIKE '%CNF%' AS literal_run_is_cnf;

-- A run broken by ANY/alt does NOT become one PIECES set (flush > 0):
-- falls back to CNF, still sound.
SELECT tre_extract_debug('ab.[cd]e') LIKE '%CNF%' AS broken_run_is_cnf;

-- A large class product exceeds the cap and stays CNF.
SELECT tre_extract_debug('[a-z][a-z][a-z][a-z]') NOT LIKE '%PIECES%' AS big_product_stays_cnf;

-- Correctness: the PIECES extraction must agree with a sequential scan.
DROP TABLE IF EXISTS cdt;
CREATE TABLE cdt (id int, s text);
INSERT INTO cdt VALUES
  (1,'xaabcbx'), (2,'yaaccdy'), (3,'aabccz'),         -- true matches
  (4,'aab...acc...bcd'), (5,'has aab and acc and ccb apart'),  -- CNF false positives
  (6,'nothing here'), (7,'aaccc');
INSERT INTO cdt SELECT g, 'filler '||g||' aa bc cb' FROM generate_series(100,600) g;
CREATE INDEX cdt_tre ON cdt USING tre (s);
ANALYZE cdt;

CREATE FUNCTION cdt_agree(pat text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a int[]; b int[];
BEGIN
  PERFORM set_config('enable_seqscan','off',true);
  PERFORM set_config('enable_bitmapscan','on',true);
  PERFORM set_config('enable_indexscan','off',true);
  EXECUTE 'SELECT array_agg(id ORDER BY id) FROM cdt WHERE s %~~ tre_pattern($1,0)' INTO a USING pat;
  PERFORM set_config('enable_seqscan','on',true);
  PERFORM set_config('enable_bitmapscan','off',true);
  EXECUTE 'SELECT array_agg(id ORDER BY id) FROM cdt WHERE tre_amatch(s,$1,0)' INTO b USING pat;
  RETURN a IS NOT DISTINCT FROM b;
END $$;

SELECT bool_and(cdt_agree(p)) AS all_class_patterns_agree
FROM (VALUES
  ('aa[bc]c[b-d]'), ('a[bc]c'), ('[ab][cd][ef]'), ('x[0-9]z'),
  ('ab.[cd]e'), ('[a-z][a-z][a-z][a-z]'), ('abc[de]f')) v(p);

DROP TABLE cdt;
DROP FUNCTION cdt_agree(text);
