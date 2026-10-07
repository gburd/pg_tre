-- test/sql/tre_collation_classes.sql
--
-- TRE's character classes ([[:alpha:]], \w ...) and case folding ((?i))
-- follow the collation of the call, as core's ~ / ~* do
-- (pg_set_regex_collation): the input collation of tre_amatch* /
-- tre_similarity / tre_distance and of the %~~ and <@> operators, and the
-- index column's collation for an index scan.  Everything here runs in
-- ONE database with explicit collations only, so the output is the same
-- whatever the database default is (CI runs the suite under C and
-- C.UTF-8).  Checked, per collation: every class and every case pair
-- against core; %~~ through a sequential scan, a bitmap index scan (the
-- executor's recheck) and an ordered index scan (ORDER BY <@>, which
-- matches inside the index AM); "C" forcing ASCII; a nondeterministic or
-- indeterminate collation erroring as core does; and the compiled-pattern
-- cache never handing a pattern compiled under one collation to another.
--
-- Under ICU, core classifies with libicu and pg_tre with PostgreSQL's
-- Unicode tables (LIMITATIONS.md; tre_ctype_strategies); the characters
-- below avoid the few where those differ.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_tre;
\set VERBOSITY terse

DROP TABLE IF EXISTS tcc_ch, tcc_coll, tcc;
DROP COLLATION IF EXISTS tcc_nd;

CREATE TABLE tcc_ch (ord serial, c text, letter bool);
INSERT INTO tcc_ch (c, letter) VALUES
  ('a', true), ('Z', true), ('5', false), ('_', false), (' ', false),
  ('é', true), ('É', true), ('ß', true), ('ÿ', true), ('Ω', true),
  ('ω', true), ('я', true), ('Я', true), ('日', true), ('ｆ', true),
  ('Ａ', true), (chr(160), false), (chr(8195), false), ('!', false);

-- Collations under test: one per strategy.  The libc one needs the OS
-- C.UTF-8 locale; it prints nothing different when missing.
CREATE TABLE tcc_coll (coll text);
INSERT INTO tcc_coll SELECT c FROM unnest(ARRAY['C', 'POSIX', 'und-x-icu',
                                                'pg_c_utf8', 'C.utf8']) c
 WHERE EXISTS (SELECT FROM pg_collation WHERE collname = c);

CREATE FUNCTION tcc_check(coll text, OUT checks int, OUT members int,
                          OUT disagree text)
LANGUAGE plpgsql AS $f$
DECLARE r record; core bool; k0 bool; k1 bool; bad text[] := '{}';
BEGIN
  checks := 0; members := 0;
  FOR r IN
    SELECT ch.c, ch.ord, p.re, p.label
      FROM tcc_ch ch,
           (VALUES ('^[[:alpha:]]$', 'alpha'), ('^[[:upper:]]$', 'upper'),
                   ('^[[:lower:]]$', 'lower'), ('^[[:alnum:]]$', 'alnum'),
                   ('^[[:space:]]$', 'space'), ('^[[:blank:]]$', 'blank'),
                   ('^[^[:alpha:]]$', '^alpha'), ('^\w$', '\w'),
                   ('^\W$', '\W'), ('^\s$', '\s'), ('^\d$', '\d'),
                   ('(?i)^[[:upper:]]$', '(?i)upper')) AS p(re, label)
     ORDER BY p.label, ch.ord
  LOOP
    EXECUTE format('SELECT %L COLLATE %I ~ %L, tre_amatch(%L COLLATE %I, %L, 0),
                           coalesce(tre_amatch_cost(%L COLLATE %I, %L, 1), 1) = 0',
                   r.c, coll, r.re, r.c, coll, r.re, r.c, coll, r.re)
      INTO core, k0, k1;
    checks := checks + 1;
    IF core THEN members := members + 1; END IF;
    IF k0 IS DISTINCT FROM core OR k1 IS DISTINCT FROM core THEN
      bad := bad || (r.label || ':' || r.c);
    END IF;
  END LOOP;
  -- case folding, every ordered pair of letters
  FOR r IN SELECT a.c AS a, b.c AS b FROM tcc_ch a, tcc_ch b
            WHERE a.letter AND b.letter ORDER BY a.ord, b.ord
  LOOP
    EXECUTE format('SELECT %L COLLATE %I ~* %L, tre_amatch(%L COLLATE %I, %L, 0)',
                   r.a, coll, '^' || r.b || '$', r.a, coll, '(?i)^' || r.b || '$')
      INTO core, k0;
    checks := checks + 1;
    IF core THEN members := members + 1; END IF;
    IF k0 IS DISTINCT FROM core THEN
      bad := bad || ('fold:' || r.a || '/' || r.b);
    END IF;
  END LOOP;
  disagree := coalesce(nullif(array_to_string(bad, ' '), ''), 'none');
END $f$;

-- (1) Classes and folding follow the call's collation.  The members
--     column differs per strategy (C/POSIX classify ASCII only), so a
--     collation silently falling back to the database default shows up.
SELECT coll, (tcc_check(coll)).* FROM tcc_coll WHERE coll <> 'C.utf8' ORDER BY coll;
SELECT coll, (tcc_check(coll)).disagree AS libc_disagree
  FROM tcc_coll WHERE coll = 'C.utf8' AND (tcc_check(coll)).disagree <> 'none';

-- (2) COLLATE "C" forces ASCII; the same expression, the same answer as ~.
SELECT tre_amatch('é' COLLATE "C", '^[[:alpha:]]$', 0) AS tre_c,
       'é' COLLATE "C" ~ '^[[:alpha:]]$' AS core_c,
       tre_amatch('é' COLLATE "und-x-icu", '^[[:alpha:]]$', 0) AS tre_icu,
       'é' COLLATE "und-x-icu" ~ '^[[:alpha:]]$' AS core_icu,
       tre_amatch('É' COLLATE "C", '(?i)^é$', 0) AS fold_c,
       tre_amatch('É' COLLATE "und-x-icu", '(?i)^é$', 0) AS fold_icu;
-- ... through every SQL entry point
SELECT tre_amatch_cost('é' COLLATE "C", '^[[:alpha:]]$', 0) AS cost_c,
       tre_amatch_cost('é' COLLATE "und-x-icu", '^[[:alpha:]]$', 0) AS cost_icu,
       tre_amatch('é' COLLATE "C", '^[[:alpha:]]$', 0, 1, 1, 1) AS costs_c,
       tre_amatch('é' COLLATE "und-x-icu", '^[[:alpha:]]$', 0, 1, 1, 1) AS costs_icu,
       (SELECT count(*) FROM tre_amatch_detail('é' COLLATE "C", '^[[:alpha:]]$', 0)) AS detail_c,
       (SELECT count(*) FROM tre_amatch_detail('é' COLLATE "und-x-icu", '^[[:alpha:]]$', 0)) AS detail_icu,
       tre_distance('É' COLLATE "C", '(?i)^é$', 0) AS dist_c,
       tre_distance('É' COLLATE "und-x-icu", '(?i)^é$', 0) AS dist_icu,
       tre_similarity('É' COLLATE "C", '(?i)^é$', 0) AS sim_c,
       tre_similarity('É' COLLATE "und-x-icu", '(?i)^é$', 0) AS sim_icu,
       tre_distance('É' COLLATE "C", tre_pattern('(?i)^é$', 0)) AS pdist_c,
       tre_distance('É' COLLATE "und-x-icu", tre_pattern('(?i)^é$', 0)) AS pdist_icu,
       'é' COLLATE "C" %~~ tre_pattern('^[[:alpha:]]$', 0) AS op_c,
       'é' COLLATE "und-x-icu" %~~ tre_pattern('^[[:alpha:]]$', 0) AS op_icu;

-- (3) Through the index.  One table, a column per collation, a tre index
--     on each; every pattern via %~~ must give core's ~ / ~* answer on
--     the same column, by sequential scan, bitmap index scan (recheck)
--     and ordered index scan (ORDER BY <@>, matched inside the AM).
CREATE TABLE tcc (id serial, s_icu text COLLATE "und-x-icu", s_c text COLLATE "C");
INSERT INTO tcc (s_icu) SELECT 'abcxq' || c || 'zwdef' FROM tcc_ch ORDER BY ord;
INSERT INTO tcc (s_icu) SELECT 'filler ' || g FROM generate_series(1, 300) g;
UPDATE tcc SET s_c = s_icu;
CREATE INDEX tcc_icu ON tcc USING tre (s_icu);
CREATE INDEX tcc_c ON tcc USING tre (s_c);
INSERT INTO tcc (s_icu, s_c) SELECT 'pre abcxq' || c || 'zwdef', 'pre abcxq' || c || 'zwdef'
  FROM tcc_ch ORDER BY ord;
ANALYZE tcc;

CREATE FUNCTION tcc_ids(q text, mode text) RETURNS int[] LANGUAGE plpgsql AS $f$
DECLARE v int[]; plan text := ''; l text;
BEGIN
  PERFORM set_config('enable_seqscan', CASE WHEN mode = 'seq' THEN 'on' ELSE 'off' END, true);
  PERFORM set_config('enable_bitmapscan', CASE WHEN mode = 'bitmap' THEN 'on' ELSE 'off' END, true);
  PERFORM set_config('enable_indexscan', CASE WHEN mode = 'ordered' THEN 'on' ELSE 'off' END, true);
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || ' '; END LOOP;
  IF (mode = 'seq' AND plan NOT LIKE '%Seq Scan%')
     OR (mode = 'ordered' AND plan NOT LIKE '%Index Scan using%')
     OR (mode = 'bitmap' AND plan NOT LIKE '%Bitmap Index Scan%') THEN
    RAISE EXCEPTION 'plan for % is not %: %', q, mode, plan;
  END IF;
  EXECUTE 'SELECT array_agg(id ORDER BY id) FROM (' || q || ') x' INTO v;
  RETURN v;
END $f$;

WITH p(col, re, k) AS (VALUES
  ('s_icu', 'abcxq[[:alpha:]]zwdef', 0), ('s_c', 'abcxq[[:alpha:]]zwdef', 0),
  ('s_icu', 'abcxq[[:upper:]]zwdef', 0), ('s_c', 'abcxq[[:upper:]]zwdef', 0),
  ('s_icu', 'abcxq\wzwdef', 0),          ('s_c', 'abcxq\wzwdef', 0),
  ('s_icu', 'abcxq[^[:alnum:]]zwdef', 0), ('s_c', 'abcxq[^[:alnum:]]zwdef', 0),
  ('s_icu', '(?i)ABCXQÉZWDEF', 0),       ('s_c', '(?i)ABCXQÉZWDEF', 0),
  ('s_icu', '(?i)abcxqωzwdef', 0),       ('s_c', '(?i)abcxqωzwdef', 0),
  ('s_icu', 'abcxq[[:alpha:]]zwdeX', 1), ('s_c', 'abcxq[[:alpha:]]zwdeX', 1)),
q AS (SELECT col, re, k,
             format('SELECT id FROM tcc WHERE %I %%~~ tre_pattern(%L, %s)', col, re, k) AS q_where,
             format('SELECT id FROM tcc WHERE %I %%~~ tre_pattern(%L, %s) ORDER BY %I <@> tre_pattern(%L, %s) LIMIT 1000',
                    col, re, k, col, re, k) AS q_order,
             format('SELECT id FROM tcc WHERE %I %s %L', col,
                    CASE WHEN re LIKE '(?i)%' THEN '~*' ELSE '~' END,
                    CASE WHEN k = 1 THEN NULL ELSE regexp_replace(re, '^\(\?i\)', '') END) AS q_core
        FROM p),
r AS (SELECT col, re, k, tcc_ids(q_where, 'seq') AS seq, tcc_ids(q_where, 'bitmap') AS bitmap,
             tcc_ids(q_order, 'ordered') AS ordered,
             CASE WHEN k = 0 THEN tcc_ids(q_core, 'seq') END AS core
        FROM q)
SELECT col, re, k, cardinality(seq) AS rows,
       seq IS NOT DISTINCT FROM bitmap AND seq IS NOT DISTINCT FROM ordered AS idx_eq_seq,
       CASE WHEN k = 0 THEN seq IS NOT DISTINCT FROM core END AS eq_core
  FROM r ORDER BY re, col;

-- (4) A nondeterministic collation is refused, as by core; so is an
--     indeterminate one (two columns, implicit collations in conflict).
CREATE COLLATION tcc_nd (provider = icu, locale = 'und-u-ks-level2',
                         deterministic = false);
SELECT 'a' COLLATE tcc_nd ~ 'a';
SELECT tre_amatch('a' COLLATE tcc_nd, 'a', 0);
SELECT tre_distance('a' COLLATE tcc_nd, 'a', 0);
SELECT 'a' COLLATE tcc_nd %~~ tre_pattern('a', 0);
SELECT count(*) FROM tcc WHERE s_icu ~ s_c;
SELECT count(*) FROM tcc WHERE tre_amatch(s_icu, s_c, 0);
-- ... and an error leaves nothing behind: after a refused collation, an
--     invalid pattern and a match timeout, each under another collation,
--     the next calls classify by their own collation.
SELECT tre_amatch('é' COLLATE "und-x-icu", '^[[:alpha:]]$', 0) AS icu;
SELECT tre_amatch('é' COLLATE tcc_nd, '^[[:alpha:]]$', 0);
SELECT tre_amatch('é' COLLATE "C", '^[[:alpha:]]$', 0) AS c,
       tre_amatch('é' COLLATE "und-x-icu", '^[[:alpha:]]$', 0) AS icu;
SELECT tre_amatch('é' COLLATE "C", '[[:nosuch:]]', 0);
SELECT tre_amatch('é' COLLATE "und-x-icu", '^[[:alpha:]]$', 0) AS icu;
SET pg_tre.match_timeout_ms = 1;
SELECT tre_amatch(repeat('é', 200000) COLLATE "C", '(\w|é)*x', 3);
RESET pg_tre.match_timeout_ms;
SELECT tre_amatch('é' COLLATE "und-x-icu", '^[[:alpha:]]$', 0) AS icu,
       tre_amatch('é' COLLATE "C", '^[[:alpha:]]$', 0) AS c;

-- (5) The compiled-pattern cache keys on the collation.  (?i) case
--     counterparts are computed when the pattern is COMPILED, so a
--     pattern compiled under "C" (é has no other case) reused under ICU
--     (é ~ É) answers wrongly.  Same pattern text, collations
--     alternating, one session.
SELECT g, CASE WHEN g % 2 = 0
               THEN tre_amatch('É' COLLATE "C", '(?i)^é$', 0)
               ELSE tre_amatch('É' COLLATE "und-x-icu", '(?i)^é$', 0) END AS tre,
          CASE WHEN g % 2 = 0
               THEN 'É' COLLATE "C" ~* '^é$'
               ELSE 'É' COLLATE "und-x-icu" ~* '^é$' END AS core
  FROM generate_series(1, 6) g ORDER BY g;
SELECT count(*) AS calls,
       count(*) FILTER (WHERE tre IS DISTINCT FROM core) AS wrong
  FROM (SELECT CASE WHEN g % 3 = 0 THEN tre_amatch('xÉy' COLLATE "C", '(?i)x[é]y|^[[:alpha:]]$', 0)
                    WHEN g % 3 = 1 THEN tre_amatch('xÉy' COLLATE "und-x-icu", '(?i)x[é]y|^[[:alpha:]]$', 0)
                    ELSE tre_amatch('xÉy' COLLATE "pg_c_utf8", '(?i)x[é]y|^[[:alpha:]]$', 0) END AS tre,
               CASE WHEN g % 3 = 0 THEN 'xÉy' COLLATE "C" ~* 'x[é]y|^[[:alpha:]]$'
                    WHEN g % 3 = 1 THEN 'xÉy' COLLATE "und-x-icu" ~* 'x[é]y|^[[:alpha:]]$'
                    ELSE 'xÉy' COLLATE "pg_c_utf8" ~* 'x[é]y|^[[:alpha:]]$' END AS core
          FROM generate_series(1, 90) g) x;

DROP FUNCTION tcc_ids(text, text);
DROP FUNCTION tcc_check(text);
DROP TABLE tcc, tcc_ch, tcc_coll;
DROP COLLATION tcc_nd;
