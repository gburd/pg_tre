-- test/sql/regex_groups_index.sql
--
-- Patterns core's regex accepts must work through the index, with the same
-- rows as a sequential scan, and with trigrams where the pattern has them.
--
-- pg_tre's own pattern parser exists only to find trigrams for the index;
-- the recheck's engine (core regex for ~, TRE for %~~) decides matches.
-- Before 4.3.1 that parser rejected patterns it did not know -- lazy
-- quantifiers ('x*?'), empty alternatives ('(|a)'), director prefixes
-- ('***='), '{,3}' -- with "pg_tre: invalid regex pattern:" and an empty
-- message, only on the index path.  Lookarounds and '(?:...)' were scanned
-- with no trigrams at all.  Reported by depesz for '(?<=e.)aa[bc]c[b-d]'.
-- Now: '(?:X)' is a group, a lookaround stands in for an optional
-- character, a lazy quantifier is its greedy twin, and anything else the
-- parser cannot read is scanned lossily and left to the recheck.

CREATE EXTENSION IF NOT EXISTS pg_tre;
SET client_min_messages = warning;

DROP TABLE IF EXISTS rgx;
CREATE TABLE rgx (id int, s text);
INSERT INTO rgx VALUES
  (1, 'xe.aacbz'), (2, 'xf.aacbz'), (3, 'the nation'), (4, 'motion'),
  (5, 'fooxbar'), (6, 'foo bar'), (7, '{,3}'), (8, 'a{,3}b'),
  (9, 'tion'), (10, 'ab'), (11, 'abc)def'), (12, 'xx(ab'),
  (13, '***=lit'), (14, 'aaa'), (15, 'nation-wide');
INSERT INTO rgx SELECT g, 'filler ' || g FROM generate_series(100, 1100) g;
CREATE INDEX rgx_tre ON rgx USING tre (s);
ANALYZE rgx;

CREATE TEMP TABLE pats (p text);
INSERT INTO pats VALUES
  ('(?<=e.)aa[bc]c[b-d]'), ('(?<=e\.)aac'), ('(?<!f.)aacb'),
  ('(?=tion)tio'), ('na(?=tion)tion'), ('(?!x)motion'), ('mo(?!x)tion'),
  ('(?<=[a-z])ation'), ('(?<=(n|m))ation'), ('foo(?=[x ])'),
  ('(?:na)tion'), ('(?:fo)+o.bar'), ('(?:a|b)c\)def'),
  ('x*?tion'), ('fo+?x'), ('ab{1,3}?'), ('tio??n'),
  ('a{,3}'), ('()tion'), ('(|a)tion'), ('a||b'), ('***=lit'), ('***:tion'),
  ('(?i)TION');

-- mode: 'bitmap', 'index' (plain Index Scan) or 'seq'.
CREATE FUNCTION rgx_rows(pat text, mode text) RETURNS int[]
LANGUAGE plpgsql AS $$
DECLARE r int[];
BEGIN
  PERFORM set_config('enable_seqscan',    (mode = 'seq')::text,    true);
  PERFORM set_config('enable_bitmapscan', (mode = 'bitmap')::text, true);
  PERFORM set_config('enable_indexscan',  (mode = 'index')::text,  true);
  EXECUTE 'SELECT array_agg(id ORDER BY id) FROM rgx WHERE s ~ $1'
    INTO r USING pat;
  RETURN r;
END $$;

SELECT p, cardinality(rgx_rows(p, 'seq')) AS n_rows,
       rgx_rows(p, 'bitmap') IS NOT DISTINCT FROM rgx_rows(p, 'seq') AS bitmap_ok,
       rgx_rows(p, 'index') IS NOT DISTINCT FROM rgx_rows(p, 'seq') AS index_ok
  FROM pats ORDER BY p;

-- Trigrams are still extracted around a lookaround or (?:...): the index
-- narrows the scan instead of reading every heap block.
SELECT p, tre_extract_debug(p) NOT LIKE '%ALWAYS_TRUE%' AS has_trigrams
  FROM (VALUES ('(?<=e\.)aacb'), ('na(?=tion)tion'), ('(?:na)tion'),
               ('x*?tion')) v(p);

-- A pattern no engine accepts still fails, with the engine's message.
SET enable_seqscan = off;
SELECT count(*) FROM rgx WHERE s ~ 'a(b';
SELECT count(*) FROM rgx WHERE s %~~ tre_pattern('(?<=e.)aac', 0);
RESET enable_seqscan;

DROP TABLE rgx;
