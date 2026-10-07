-- test/sql/collation_matrix.sql
--
-- pg_tre refuses to build an index under a nondeterministic collation
-- (src/am/ambuild.c): such a collation makes = / LIKE / ~ match strings
-- that share no trigrams.  This covers every way the index collation can
-- be chosen, and every command that (re)builds an index -- and checks that
-- deterministic collations (C, POSIX, ICU, builtin, libc) are accepted and
-- the index then answers exactly like a sequential scan.
--
-- Needs ICU (the ICU collations are created here).

CREATE EXTENSION IF NOT EXISTS pg_tre;
\set VERBOSITY terse
SET client_min_messages = warning;

DROP TABLE IF EXISTS cm, cm2, cm_dom, cm_expr CASCADE;
DROP DOMAIN IF EXISTS cm_ci_text;
DROP COLLATION IF EXISTS cm_ci;
DROP COLLATION IF EXISTS cm_ai;
DROP COLLATION IF EXISTS cm_icu_det;
CREATE COLLATION cm_ci (provider = icu, locale = 'und-u-ks-level2',
                        deterministic = false);   -- case-insensitive
CREATE COLLATION cm_ai (provider = icu, locale = 'und-u-ks-level1-kc-true',
                        deterministic = false);   -- accent-insensitive
CREATE COLLATION cm_icu_det (provider = icu, locale = 'und');

-- Index vs sequential scan, for a set of queries over table cm(s).
CREATE FUNCTION cm_ids(tab regclass, q text, ix bool) RETURNS int[]
LANGUAGE plpgsql AS $f$
DECLARE v int[];
BEGIN
  PERFORM set_config('enable_seqscan', CASE WHEN ix THEN 'off' ELSE 'on' END, true);
  PERFORM set_config('enable_indexscan', CASE WHEN ix THEN 'on' ELSE 'off' END, true);
  PERFORM set_config('enable_bitmapscan', CASE WHEN ix THEN 'on' ELSE 'off' END, true);
  EXECUTE format('SELECT array_agg(id ORDER BY id) FROM %s WHERE %s', tab, q) INTO v;
  RETURN v;
END $f$;
CREATE FUNCTION cm_check(tab regclass, OUT queries int, OUT idx_ne_seq int,
                         OUT nonempty int)
LANGUAGE sql AS $f$
  SELECT count(*)::int,
         count(*) FILTER (WHERE cm_ids(tab, q, true) IS DISTINCT FROM cm_ids(tab, q, false))::int,
         count(*) FILTER (WHERE cm_ids(tab, q, false) IS NOT NULL)::int
    FROM unnest(ARRAY[
      $$s %~~ tre_pattern('café', 0)$$, $$s %~~ tre_pattern('cafe', 1)$$,
      $$s LIKE '%afé%'$$, $$s ILIKE '%CAFÉ%'$$, $$s ~ 'caf[eé]'$$,
      $$s ~* 'CAF'$$, $$s = 'Café crème'$$, $$s %~~ tre_pattern('crème', 0)$$]) q
$f$;

CREATE TABLE cm (id serial, s text);
INSERT INTO cm (s) VALUES ('Café crème'), ('cafe creme'), ('CAFÉ'), ('unrelated');
INSERT INTO cm (s) SELECT 'filler ' || g FROM generate_series(1, 300) g;

-- (1) Column collation, both nondeterministic kinds: refused.
CREATE TABLE cm2 (id serial, s text COLLATE cm_ci);
INSERT INTO cm2 (s) VALUES ('Café');
CREATE INDEX cm2_bad ON cm2 USING tre (s);
ALTER TABLE cm2 ALTER COLUMN s TYPE text COLLATE cm_ai;
CREATE INDEX cm2_bad ON cm2 USING tre (s);
-- ... but an explicit deterministic index collation overrides it.
CREATE INDEX cm2_c ON cm2 USING tre (s COLLATE "C");
DROP INDEX cm2_c;
CREATE INDEX cm2_posix ON cm2 USING tre (s COLLATE "POSIX");
DROP TABLE cm2;

-- (2) CREATE INDEX ... COLLATE nondeterministic on a deterministic column.
CREATE INDEX cm_bad ON cm USING tre (s COLLATE cm_ci);
CREATE INDEX CONCURRENTLY cm_bad ON cm USING tre (s COLLATE cm_ci);
-- A failed CONCURRENTLY build leaves an invalid index behind (as for any
-- AM); it still carries the collation, so drop it before going on.
SELECT indexrelid::regclass AS idx, indisvalid
  FROM pg_index WHERE indrelid = 'cm'::regclass;
DROP INDEX cm_bad;

-- (3) A domain over text with a nondeterministic collation.
CREATE DOMAIN cm_ci_text AS text COLLATE cm_ci;
CREATE TABLE cm_dom (id serial, s cm_ci_text);
INSERT INTO cm_dom (s) VALUES ('Café');
CREATE INDEX cm_dom_bad ON cm_dom USING tre (s);
CREATE INDEX cm_dom_c ON cm_dom USING tre ((s::text) COLLATE "C");
DROP TABLE cm_dom;

-- (4) Expression indexes: the expression's collation decides.
CREATE TABLE cm_expr (id serial, s text COLLATE cm_ci);
INSERT INTO cm_expr (s) VALUES ('Café'), ('x');
CREATE INDEX cm_expr_bad ON cm_expr USING tre (lower(s));
CREATE INDEX cm_expr_bad ON cm_expr USING tre ((s || ''));
CREATE INDEX cm_expr_ok ON cm_expr USING tre ((lower(s) COLLATE "C"));
SELECT count(*) AS expr_indexes FROM pg_index WHERE indrelid = 'cm_expr'::regclass;
DROP TABLE cm_expr;

-- (5) Deterministic collations are accepted, and the index answers like a
--     seq scan -- with rows from the build and from aminsert.
CREATE INDEX cm_default ON cm USING tre (s);
INSERT INTO cm (s) VALUES ('le café crème'), ('CAFE');
SELECT 'default' AS collation, * FROM cm_check('cm');
DROP INDEX cm_default;
CREATE INDEX cm_icu ON cm USING tre (s COLLATE cm_icu_det);
SELECT 'icu und (index)' AS collation, * FROM cm_check('cm');
DROP INDEX cm_icu;
ALTER TABLE cm ALTER COLUMN s TYPE text COLLATE "und-x-icu";
CREATE INDEX cm_icu2 ON cm USING tre (s);
SELECT 'und-x-icu (column)' AS collation, * FROM cm_check('cm');
-- (6) ALTER COLUMN to a nondeterministic collation rebuilds the index:
--     refused, and nothing changes.
ALTER TABLE cm ALTER COLUMN s TYPE text COLLATE cm_ci;
SELECT collation_name FROM information_schema.columns
 WHERE table_name = 'cm' AND column_name = 's';
DROP INDEX cm_icu2;
ALTER TABLE cm ALTER COLUMN s TYPE text COLLATE "C";
CREATE INDEX cm_c ON cm USING tre (s);
SELECT 'C (column)' AS collation, * FROM cm_check('cm');
DROP INDEX cm_c;
ALTER TABLE cm ALTER COLUMN s TYPE text COLLATE "POSIX";
CREATE INDEX cm_posix ON cm USING tre (s);
SELECT 'POSIX (column)' AS collation, * FROM cm_check('cm');
DROP INDEX cm_posix;
ALTER TABLE cm ALTER COLUMN s TYPE text COLLATE "ucs_basic";
CREATE INDEX cm_ucs ON cm USING tre (s);
SELECT 'ucs_basic (column)' AS collation, * FROM cm_check('cm');
DROP INDEX cm_ucs;
ALTER TABLE cm ALTER COLUMN s TYPE text COLLATE "default";

-- (7) REINDEX and CREATE INDEX CONCURRENTLY run the same check.  An index
--     cannot be created under a nondeterministic collation, so simulate a
--     catalog that says otherwise by redefining the collation the index
--     uses: drop and recreate cm_flip with a different determinism.
DROP COLLATION IF EXISTS cm_flip;
CREATE COLLATION cm_flip (provider = icu, locale = 'und');
CREATE INDEX cm_flip_idx ON cm USING tre (s COLLATE cm_flip);
SELECT 'cm_flip (index)' AS collation, * FROM cm_check('cm');
-- PostgreSQL never changes a collation's determinism in place, but an
-- upgrade from a pg_tre without the guard can carry such an index; mimic
-- it (superuser only, catalog edit, rolled back).
BEGIN;
UPDATE pg_collation SET collisdeterministic = false WHERE collname = 'cm_flip';
REINDEX INDEX cm_flip_idx;
ROLLBACK;
BEGIN;
UPDATE pg_collation SET collisdeterministic = false WHERE collname = 'cm_flip';
REINDEX TABLE cm;
ROLLBACK;
BEGIN;
UPDATE pg_collation SET collisdeterministic = false WHERE collname = 'cm_flip';
CREATE INDEX cm_flip_idx2 ON cm USING tre (s COLLATE cm_flip);
ROLLBACK;
UPDATE pg_collation SET collisdeterministic = false WHERE collname = 'cm_flip';
REINDEX INDEX CONCURRENTLY cm_flip_idx;
CREATE INDEX CONCURRENTLY cm_flip_cic ON cm USING tre (s COLLATE cm_flip);
UPDATE pg_collation SET collisdeterministic = true WHERE collname = 'cm_flip';
SELECT indexrelid::regclass AS idx, indisvalid
  FROM pg_index WHERE indrelid = 'cm'::regclass ORDER BY 1::text;
DROP INDEX cm_flip_cic;
DROP INDEX cm_flip_idx_ccnew;
SELECT 'cm_flip after' AS collation, * FROM cm_check('cm');
DROP INDEX cm_flip_idx;
DROP COLLATION cm_flip;

DROP TABLE cm;
DROP FUNCTION cm_check(regclass);
DROP FUNCTION cm_ids(regclass, text, bool);
DROP DOMAIN cm_ci_text;
DROP COLLATION cm_ci;
DROP COLLATION cm_ai;
DROP COLLATION cm_icu_det;
