-- test/sql/regex_syntax_edges.sql
--
-- Edge cases of pg_tre's own regex tokenizer (src/query/tokens.c), which
-- extracts trigrams for the index and must agree with TRE (the recheck):
-- every escape kind, braces ({m,n} and approximate {~k}), bracket corner
-- cases, embedded options, multibyte characters after a backslash, and the
-- syntax errors.  Errors are printed; every valid pattern is run through
-- the index and through a sequential scan, which must agree.  Plus the
-- TRE decoder corner cases reachable from SQL: an empty subject, and
-- k=0 / k>=1 / back-reference matching over strings whose LAST character
-- is multibyte (the decoder sees exactly the remaining bytes).  Known
-- upstream-TRE limitations visible below: (é)\1 finds nothing in a
-- multibyte database (the backtracker compares a back-reference one byte
-- short), and TRE has no \0nnn / \U escapes or [[.x.]] elements.

CREATE EXTENSION IF NOT EXISTS pg_tre;
\set VERBOSITY terse
SET client_min_messages = warning;

DROP TABLE IF EXISTS rse;
CREATE TABLE rse (id serial, s text);
INSERT INTO rse (s) VALUES
  ('abc\'), ('a{2'), ('aa'), ('ab]b'), ('a^b'), ('café'), ('caf\é'),
  ('aaab'), ('cafA'), (E'x\ny'), ('[ab]'), ('-a'), ('a-'), ('a.b'),
  ('xyzé'), ('ééé'), ('abcabcé'), ('(?i)abc'), ('ABC');
INSERT INTO rse (s) SELECT 'filler ' || g FROM generate_series(1, 200) g;
CREATE INDEX rse_tre ON rse USING tre (s);
INSERT INTO rse (s) VALUES ('pre café post'), ('pre aaab'), ('pre ABC');

-- Syntax errors (the tokenizer's, on the index path and in the debug
-- functions alike).
SELECT tre_parse_debug('abc\');
SELECT tre_parse_debug('[a\');
SELECT tre_parse_debug('a{x}');
SELECT tre_parse_debug('a{~x}');
SELECT tre_parse_debug('a{,2}');
SELECT tre_parse_debug('x[[:alpha');
SELECT tre_parse_debug('x[[=e');
SELECT tre_parse_debug('x[[.e');
SELECT tre_parse_debug('a{2');
SELECT tre_parse_debug('a(');
SELECT tre_parse_debug('[a-');
SELECT tre_parse_debug('x[a[');
SELECT tre_parse_debug('[a^b]');
SELECT tre_parse_debug('[]^a]');
SELECT tre_parse_debug('a{~1~}');
SELECT tre_amatch('cafa', 'caf[[.a.]]', 0);
SELECT tre_amatch('xa', 'x[[:al:pha:]]', 0);
SELECT tre_parse_debug('x[[:al:pha:]]') IS NOT NULL AS tokenizer_accepts;
SET enable_seqscan = off;
SELECT id FROM rse WHERE s %~~ tre_pattern('x[[:alpha', 0);
SELECT id FROM rse WHERE s ~ 'abc\';
RESET enable_seqscan;

CREATE FUNCTION rse_ids(q text, ix bool) RETURNS int[] LANGUAGE plpgsql AS $f$
DECLARE v int[];
BEGIN
  PERFORM set_config('enable_seqscan', CASE WHEN ix THEN 'off' ELSE 'on' END, true);
  PERFORM set_config('enable_indexscan', CASE WHEN ix THEN 'on' ELSE 'off' END, true);
  PERFORM set_config('enable_bitmapscan', CASE WHEN ix THEN 'on' ELSE 'off' END, true);
  EXECUTE 'SELECT array_agg(id ORDER BY id) FROM rse WHERE ' || q INTO v;
  RETURN v;
END $f$;

-- Valid patterns: index == seq scan, and the rows found.
SELECT p, k, rse_ids(format('s %%~~ tre_pattern(%L, %s)', p, k), false) AS seq,
       rse_ids(format('s %%~~ tre_pattern(%L, %s)', p, k), true)
         IS NOT DISTINCT FROM
       rse_ids(format('s %%~~ tre_pattern(%L, %s)', p, k), false) AS idx_eq_seq
  FROM (VALUES
    ('abc\\', 0), ('a\{2', 0), ('a{2}', 0), ('a{1,2}b', 0), ('a{2,}b', 0),
    ('(aa){~1}b', 0), ('caf{~1}', 0), ('ab\]b', 0), ('a\^b', 0), ('caf\é', 0),
    ('caf\\\é', 0), ('[\é]', 0), ('caf[\é]', 0), ('caf\x{e9}', 0), ('caf\U000000e9', 0),
    ('caf\0351', 0), ('caf[\x41]', 0), ('x[\n]y', 0), ('x\ny', 0), ('\[ab\]', 0),
    ('[-a]a', 0), ('a[a-]', 0), ('[^-x]a', 0), ('[]a]b', 0), ('a[^]]b', 0),
    ('[a[b]', 0), ('\(\?i\)abc', 0),
    ('(?i)abc', 0), ('(?i)abc', 1), ('a\.b', 0), ('a.b', 1),
    ('xyz.', 0), ('xyzé$', 1), ('éé', 0), ('^éé$', 1), ('(é)\1', 0),
    ('abc(abc)?é', 0), ('é{3}', 0), ('é{~1}é', 0), ('[]-a]b', 0), ('a\#b', 0),
    ('a\~b', 0), ('abcabc', 2)) AS v(p, k)
 ORDER BY p, k;

-- TRE over the end of the string: an empty subject, and a multibyte last
-- character (decoded from exactly its own bytes) in every matcher.
SELECT tre_amatch('', 'x', 0) AS empty_k0, tre_amatch('', 'x', 1) AS empty_k1,
       tre_amatch('', '', 0) AS empty_empty, tre_amatch('', '(a)\1', 0) AS empty_backref,
       tre_amatch('xé', 'xé$', 0) AS mb_last_k0,
       tre_amatch_cost('xé', '^xe$', 1) AS mb_last_k1,
       tre_amatch('ababé', '(ab)\1é$', 0) AS mb_last_backref,
       tre_amatch('abab', '(ab)\1$', 0) AS backref_end,
       tre_amatch('xyzé', '(xyz)é{~1}', 0) AS mb_last_approx_brace;

-- (?i) on non-ASCII text depends on the database ctype (C: ASCII only);
-- whatever it is, TRE and core's ~* agree, and the index agrees too.
SELECT rse_ids($$s %~~ tre_pattern('(?i)CAFÉ', 0)$$, false)
         IS NOT DISTINCT FROM rse_ids($$s ~* 'CAFÉ'$$, false) AS icase_tre_eq_core,
       rse_ids($$s %~~ tre_pattern('(?i)CAFÉ', 0)$$, true)
         IS NOT DISTINCT FROM rse_ids($$s %~~ tre_pattern('(?i)CAFÉ', 0)$$, false) AS icase_idx_eq_seq;

-- A literal run longer than the extractor's 1024-character buffer makes
-- the k>=1 query unindexable (lossy) rather than failing.
SELECT tre_extract_debug(repeat('ab', 600), 1) ~ 'always_true' AS long_run_lossy;

-- Corrupt (undecodable) bytes in the subject make every matcher -- here
-- the back-reference one -- report no match.  The server validates text,
-- so a binary-coercible cast (superuser, rolled back) fakes the damage.
BEGIN;
CREATE CAST (bytea AS text) WITHOUT FUNCTION;
SELECT tre_amatch('\x61626162c328'::bytea::text, '(ab)\1', 0) AS br_invalid_tail,
       tre_amatch('\xc32861626162'::bytea::text, '(ab)\1', 0) AS br_invalid_head,
       tre_amatch('\x61626162c3'::bytea::text, '(ab)\1', 0) AS br_truncated_tail;
ROLLBACK;

-- Edit budgets at the extremes: negative means "exact", INT_MAX is not
-- clamped.
SELECT tre_amatch('ab', 'ab', -1) AS negative_k,
       tre_amatch_cost('ab', 'ax', 2147483647) AS int_max_k;

DROP FUNCTION rse_ids(text, bool);
DROP TABLE rse;
