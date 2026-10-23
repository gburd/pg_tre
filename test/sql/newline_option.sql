-- test/sql/newline_option.sql
--
-- The (?n) option (REG_NEWLINE for the rest of the enclosing group).
-- TRE's parser applied it to "." only: ^ and $ were decided from the
-- regcomp() flags, which an inline (?n) never reaches, and [^...] still
-- matched a newline.  So E'a\nfoo' %~~ tre_pattern('(?n)^foo', 0) was
-- false, where core's ~ says true (patches/tre-newline-assert.patch).
--
-- Every case runs through tre_amatch (the seq-scan UDF), the %~~
-- operator, a bitmap and a plain index scan, and for k = 0 also core ~
-- as the reference.  The parallel, backtracking (back reference) and
-- approximate (k > 0) matchers each decide assertions separately.

CREATE EXTENSION IF NOT EXISTS pg_tre;
\set VERBOSITY terse
SET client_min_messages = warning;

CREATE TABLE nl_cases (id int PRIMARY KEY, s text, p text, k int, want bool);
INSERT INTO nl_cases VALUES
  -- from the report
  ( 1, E'a\nfoo',     '(?n)^foo',       0, true),
  ( 2, E'a\nfoo',     '(?n)a$',         0, true),
  ( 3, E'a\nfoo',     '(?n)a.f',        0, false),
  ( 4, E'a\nfoo',     '(?n)^a',         0, true),
  -- without (?n): ^ and $ anchor the string, . and [^x] match \n
  ( 5, E'a\nfoo',     '^foo',           0, false),
  ( 6, E'a\nfoo',     'a$',             0, false),
  ( 7, E'a\nfoo',     'a.f',            0, true),
  ( 8, E'a\nfoo',     'a[^x]f',         0, true),
  -- [^...] excludes \n under (?n)
  ( 9, E'a\nfoo',     '(?n)a[^x]f',     0, false),
  (10, E'a\nfoo',     '(?n)a[^\n]f',    0, false),
  -- $ before a trailing newline, ^$ on the empty last line
  (11, E'foo\n',      '(?n)foo$',       0, true),
  (12, E'foo\n',      '(?n)^$',         0, true),
  (13, E'foo\n',      '^$',             0, false),
  -- turned off again, and scoped to a group
  (14, E'a\nfoo',     '(?n-n)^foo',     0, false),
  (15, E'a\nfoo',     '(?n:^foo)',      0, true),
  (16, E'a\nfoo',     'x|(?n:^foo)',    0, true),
  (17, E'a\nfoo',     '(?n:x)|^foo',    0, false),
  -- approximate matcher
  (18, E'a\nfoX',     '(?n)^foo',       1, true),
  (19, E'a\nfoX',     '(?n)^foo$',      1, true),
  (20, E'a\nfoX',     '^foo$',          1, false),
  -- backtracking matcher (a back reference selects it)
  (21, E'a\nfoofoo',  '(?n)^(foo)\1',   0, true),
  (22, E'a\nfoofoo',  '^(foo)\1',       0, false),
  -- multibyte text
  (23, E'é\nñandú',   '(?n)^ñandú$',    0, true),
  (24, E'é\nñandú',   '^ñandú$',        0, false),
  -- combined with (?i)
  (25, E'a\nFOO',     '(?in)^foo$',     0, true),
  (26, E'a\nFOO',     '(?i)^foo$',      0, false);

-- seq-scan UDF and operator; core ~ where it has the same syntax
SELECT id, p, k,
       tre_amatch(s, p, k) AS udf,
       s %~~ tre_pattern(p, k) AS op,
       CASE WHEN k = 0 AND p !~ '\(\?n(-n|:)' THEN s ~ p END AS core,
       (tre_amatch(s, p, k) = want AND (s %~~ tre_pattern(p, k)) = want) AS ok
FROM nl_cases ORDER BY id;

-- through the index, bitmap and plain scans
CREATE INDEX nl_cases_tre ON nl_cases USING tre (s);
CREATE FUNCTION nl_ids(bitmap bool) RETURNS int[] LANGUAGE plpgsql AS $$
DECLARE r int[];
BEGIN
  PERFORM set_config('enable_seqscan', 'off', true);
  PERFORM set_config('enable_bitmapscan', CASE WHEN bitmap THEN 'on' ELSE 'off' END, true);
  PERFORM set_config('enable_indexscan', CASE WHEN bitmap THEN 'off' ELSE 'on' END, true);
  SELECT array_agg(c.id ORDER BY c.id) INTO r FROM nl_cases c
   WHERE EXISTS (SELECT 1 FROM nl_cases t
                  WHERE t.id = c.id AND t.s %~~ tre_pattern(c.p, c.k));
  RETURN r;
END $$;
SELECT nl_ids(true)  = (SELECT array_agg(id ORDER BY id) FROM nl_cases WHERE want) AS bitmap_ok,
       nl_ids(false) = (SELECT array_agg(id ORDER BY id) FROM nl_cases WHERE want) AS indexscan_ok;

-- the index agrees with a sequential scan for a line-anchored pattern
SELECT op, seq_rows, bitmap_rows, indexscan_rows, ok
FROM pg_tre_index_check('nl_cases_tre') WHERE NOT ok;

DROP TABLE nl_cases;
DROP FUNCTION nl_ids(bool);
