-- test/sql/regex_class_index.sql
--
-- POSIX bracket classes, class escapes and embedded options through the
-- INDEX must give the same rows as a sequential scan.
--
-- Before 4.3.0 pg_tre's own pattern tokenizer (src/query/tokens.c) had no
-- [:class:] support: '[[:alpha:]]' was read as the set {[, :, a, l, p, h}
-- followed by a literal ']', so the index demanded a trigram the row did
-- not have and silently DROPPED matching rows.  \d \w \s raised "not yet
-- implemented", and "(?i)" raised an empty "invalid regex pattern" -- both
-- only on the index path, so the plan decided between a right answer and
-- an error.  Now: a collation-dependent bracket member or class escape is
-- one opaque character for trigram extraction (like '.'), and embedded
-- options make the pattern unindexable (lossy bitmap + recheck, as ILIKE).
-- ASCII only on purpose: which non-ASCII letters a class holds depends on
-- the collation (that is tested per strategy in tre_ctype_strategies.sql).

CREATE EXTENSION IF NOT EXISTS pg_tre;
SET client_min_messages = warning;

DROP TABLE IF EXISTS rci;
CREATE TABLE rci (id int, s text);
INSERT INTO rci VALUES
  (1, 'xayz'), (2, 'x yz'), (3, 'x5yz'), (4, 'XAYZ'), (5, 'x_yz'),
  (6, 'x-yz'), (7, 'x:yz'), (8, 'x]yz'), (9, 'x.yz'), (10, 'x	yz'),
  (11, 'abc123def'), (12, 'abc def'), (13, 'ABC DEF'),
  (14, 'a café b'), (15, 'ababcab'), (16, 'x<yz'), (17, E'tab\there');
INSERT INTO rci SELECT g, 'filler ' || g FROM generate_series(100, 1100) g;
CREATE INDEX rci_tre ON rci USING tre (s);
ANALYZE rci;

-- Each pattern: index rows, seq rows, and whether they agree.
CREATE TEMP TABLE pats (p text);
INSERT INTO pats VALUES
  ('x[[:alpha:]]yz'), ('x[[:digit:]]yz'), ('x[[:space:]]yz'),
  ('x[[:punct:]]yz'), ('x[[:alnum:]_]yz'), ('x[^[:alpha:]]yz'),
  ('x[[:upper:][:digit:]]yz'), ('x[[=a=]]yz'), ('x[[.-.]]yz'),
  ('x[]]yz'), ('x[]a]yz'), ('x[:]yz'),
  ('x\wyz'), ('x\dyz'), ('x\syz'), ('x\Wyz'), ('x\Dyz'), ('x\Syz'),
  ('x[\w]yz'), ('x[\d-]yz'),
  ('abc\d+def'), ('abc\s+def'), ('abc[[:digit:]]{3}def'),
  ('(?i)xayz'), ('(?i)ABC DEF'), ('(?n)abc'),
  -- Escapes the index must not read as their letter.  Each was indexed as
  -- a literal before 4.3.0 and dropped every matching row (core reads
  -- \m \M \y \A \Z as anchors, \xHH \uHHHH as character codes, \1 as
  -- a back-reference; TRE reads \< \> as anchors).
  ('caf\xe9'), ('caf\u00e9'), ('x\x5fyz'), ('caf[\xe9]'),
  ('\mabc'), ('def\M'), ('\yabc\y'), ('\Aabc'), ('def\Z'),
  ('(ab)c\1'), ('x\<yz'), ('\x41BC\s'), ('abc\ def'), ('tab\there');

CREATE FUNCTION rci_rows(pat text, use_index bool) RETURNS int[]
LANGUAGE plpgsql AS $$
DECLARE r int[];
BEGIN
  IF use_index THEN
    SET LOCAL enable_seqscan = off;
  ELSE
    SET LOCAL enable_indexscan = off;
    SET LOCAL enable_bitmapscan = off;
  END IF;
  EXECUTE 'SELECT array_agg(id ORDER BY id) FROM rci WHERE s ~ $1'
    INTO r USING pat;
  RETURN r;
END $$;

SELECT p, rci_rows(p, true) AS via_index, rci_rows(p, false) AS via_seq,
       rci_rows(p, true) IS NOT DISTINCT FROM rci_rows(p, false) AS agree
  FROM pats ORDER BY p;

-- The index path really was used (not a silent seq scan).
CREATE FUNCTION rci_uses_index(pat text) RETURNS bool
LANGUAGE plpgsql AS $$
DECLARE l text; hit bool := false;
BEGIN
  SET LOCAL enable_seqscan = off;
  FOR l IN EXECUTE format('EXPLAIN (COSTS OFF) SELECT * FROM rci WHERE s ~ %L', pat) LOOP
    hit := hit OR l LIKE '%rci_tre%';
  END LOOP;
  RETURN hit;
END $$;
SELECT bool_and(rci_uses_index(p)) AS all_via_index FROM pats;
DROP FUNCTION rci_uses_index(text);

-- Same through the tre_pattern operator, exact and fuzzy.
SET enable_seqscan = off;
SELECT array_agg(id ORDER BY id) AS op_idx
  FROM rci WHERE s %~~ tre_pattern('abc[[:digit:]]+def', 0);
SELECT array_agg(id ORDER BY id) AS op_idx_k1
  FROM rci WHERE s %~~ tre_pattern('abc\s+deg', 1);
RESET enable_seqscan;
SET enable_indexscan = off; SET enable_bitmapscan = off;
SELECT array_agg(id ORDER BY id) AS op_seq
  FROM rci WHERE s %~~ tre_pattern('abc[[:digit:]]+def', 0);
SELECT array_agg(id ORDER BY id) AS op_seq_k1
  FROM rci WHERE s %~~ tre_pattern('abc\s+deg', 1);
RESET enable_indexscan; RESET enable_bitmapscan;

-- A malformed class is still rejected (on both paths).
SET enable_seqscan = off;
SELECT count(*) FROM rci WHERE s ~ 'x[[:alphayz';
RESET enable_seqscan;

DROP FUNCTION rci_rows(text, bool);
DROP TABLE rci;
