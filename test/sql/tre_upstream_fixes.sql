-- test/sql/tre_upstream_fixes.sql
--
-- TRE matcher bugs fixed by patches/tre-upstream-fixes.patch (gburd/tre
-- PRs #1 and #2), as seen from SQL.  Each row was wrong before the patch:
--
--  1a  backtracking matcher lost the multibyte read offset on backtrack
--      (pos_add_next not saved), so match offsets were wrong and could even
--      point past the end of the string;
--  1b  a back reference's text was skipped with pointer arithmetic, leaving
--      the previous-character state wrong (\> \< ^ right after \1 failed);
--  1c  multibyte back references were compared one byte too early, so
--      (éa)\1 never matched;
--  2   approximate matching could not insert before a failed $ / \> / \b
--      (^caf$ was not within 1 edit of 'cafe');
--  3   a character-class mismatch was not costed as a substitution in
--      multibyte mode (^[[:alpha:]]$ vs '5' at k=1 failed).
--
-- Offsets from tre_amatch_detail are byte offsets in the database encoding.
-- Backreference rows only run in UTF-8 databases (the multibyte decoder);
-- the expected file assumes UTF-8, as every other multibyte test here.

CREATE EXTENSION IF NOT EXISTS pg_tre;
\set VERBOSITY terse
SET client_min_messages = warning;

SELECT getdatabaseencoding() AS enc;

-- 1a/1b/1c: back references and offsets, exact matching (k=0).
SELECT p.pat, p.subj,
       tre_amatch(p.subj, p.pat, 0) AS matched,
       d.match_start, d.match_end
  FROM (VALUES
         ('(éa)\1',   'éaéa'),      -- 1c: was no match
         ('(é)\1',    'aéé'),       -- 1a: was [4,7] (past the 5-byte end)
         ('(a)\1',    'a'),         -- was a false match
         ('(-a)\1\>', '-a-a'),      -- 1b: was no match
         ('(ab)\1é$', 'ababé'),
         ('b(c)\1',   'ébcc'),
         ('(é+)x\1',  'ééxéé')
       ) AS p(pat, subj)
  LEFT JOIN LATERAL tre_amatch_detail(p.subj, p.pat, 0) d ON true
 ORDER BY p.pat COLLATE "C";

-- 1a without back references: a pattern that makes the backtracking
-- matcher backtrack over a multibyte prefix must report correct offsets.
SELECT d.match_start, d.match_end
  FROM tre_amatch_detail('éb', '(é|x)*b', 0) d;

-- ^(foobar){~1}$ only allows an edit inside the group, so foobarx (one
-- extra character after the group) stays a non-match through the index;
-- with k=1 on the whole pattern it is cost 1 (above).

-- 2: insertions before a positional assertion (k=1).
SELECT p.pat, p.subj, tre_amatch_cost(p.subj, p.pat, 1) AS cost
  FROM (VALUES
         ('^caf$',    'cafe'),      -- was no match
         ('caf$',     'cafe'),      -- was no match
         ('^ca$',     'cac'),       -- was no match
         ('ac\>',     'cacb'),      -- was cost 2
         ('^café$',   'cafée'),
         ('^(foobar){~1}$', 'foobarx')
       ) AS p(pat, subj)
 ORDER BY p.pat COLLATE "C";

-- 3: a class mismatch is a substitution (k=1), multibyte decoder.  Only
-- collation-independent memberships here: classes follow the call's
-- collation (is 'é' alpha? not under C), see tre_collation_classes.
SELECT p.pat, p.subj, tre_amatch_cost(p.subj, p.pat, 1) AS cost
  FROM (VALUES
         ('^[[:alpha:]]$', '5'),
         ('^[[:digit:]]$', 'é'),
         ('^x[[:alpha:]]y$', 'x5y'),
         ('^[[:alpha:]]$', 'q')       -- exact (cost 0)
       ) AS p(pat, subj)
 ORDER BY p.pat COLLATE "C", p.subj COLLATE "C";

-- Through the index: the same patterns via %~~ must agree with a seq scan.
DROP TABLE IF EXISTS tuf;
CREATE TABLE tuf (id serial, s text);
INSERT INTO tuf (s) VALUES ('éaéa'), ('aéé'), ('-a-a'), ('ababé'), ('cafe'),
  ('cafée'), ('foobarx'), ('x5y'), ('5'), ('é'), ('plain filler'), ('caf');
INSERT INTO tuf (s) SELECT 'filler ' || g FROM generate_series(1, 300) g;
CREATE INDEX tuf_tre ON tuf USING tre (s);
ANALYZE tuf;
CREATE TEMP TABLE tuf_q (pat text, k int);
INSERT INTO tuf_q VALUES ('(éa)\1', 0), ('(-a)\1\>', 0), ('(ab)\1é$', 0),
  ('^caf$', 1), ('^café$', 1), ('^(foobar){~1}$', 0), ('^x[[:alpha:]]y$', 1);
SET enable_seqscan = off;
CREATE TEMP TABLE tuf_idx AS
  SELECT q.pat, q.k, array_agg(t.id ORDER BY t.id) AS ids
    FROM tuf_q q LEFT JOIN tuf t ON t.s %~~ tre_pattern(q.pat, q.k)
   GROUP BY q.pat, q.k;
RESET enable_seqscan;
SET enable_indexscan = off; SET enable_bitmapscan = off;
CREATE TEMP TABLE tuf_seq AS
  SELECT q.pat, q.k, array_agg(t.id ORDER BY t.id) AS ids
    FROM tuf_q q LEFT JOIN tuf t ON t.s %~~ tre_pattern(q.pat, q.k)
   GROUP BY q.pat, q.k;
RESET enable_indexscan; RESET enable_bitmapscan;
SELECT i.pat, i.k, i.ids AS idx_ids, i.ids IS NOT DISTINCT FROM s.ids AS idx_eq_seq
  FROM tuf_idx i JOIN tuf_seq s USING (pat, k)
 ORDER BY i.pat COLLATE "C";

DROP TABLE tuf;
