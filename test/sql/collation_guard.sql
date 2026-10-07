-- test/sql/collation_guard.sql
--
-- pg_tre refuses nondeterministic collations.  Under one (ICU with
-- strength level1: 'a' = 'A' = 'á'), = and LIKE match strings that share
-- no trigrams, so a trigram index would silently miss rows.  Every way of
-- picking the index collation is covered: the column's COLLATE, an
-- explicit CREATE INDEX ... COLLATE, and ALTER COLUMN changing it (which
-- rebuilds the index, so the check runs again).
--
-- Needs ICU; the collation is created here so the test is self-contained.

CREATE EXTENSION IF NOT EXISTS pg_tre;
\set VERBOSITY terse
SET client_min_messages = warning;


DROP TABLE IF EXISTS cg;
DROP COLLATION IF EXISTS cg_ci;
CREATE COLLATION cg_ci (provider = icu, locale = 'und-u-ks-level1',
                        deterministic = false);

-- (1) Column collation: refused.
CREATE TABLE cg (s text COLLATE cg_ci);
INSERT INTO cg VALUES ('Café'), ('cafe');
CREATE INDEX cg_bad ON cg USING tre (s);

-- (2) Index collation overrides the column: COLLATE "C" is accepted ...
CREATE INDEX cg_c ON cg USING tre (s COLLATE "C");
SELECT count(*) AS indexes FROM pg_indexes WHERE tablename = 'cg';
DROP INDEX cg_c;

-- ... and an explicit nondeterministic index collation is refused.
DROP TABLE cg;
CREATE TABLE cg (s text);
INSERT INTO cg VALUES ('Café'), ('cafe');
CREATE INDEX cg_bad ON cg USING tre (s COLLATE cg_ci);

-- (3) ALTER COLUMN to a nondeterministic collation rebuilds the index, so
--     it is refused too -- and the table is left as it was.
CREATE INDEX cg_ok ON cg USING tre (s);
ALTER TABLE cg ALTER COLUMN s TYPE text COLLATE cg_ci;
SELECT collation_name FROM information_schema.columns
 WHERE table_name = 'cg' AND column_name = 's';

-- (4) A deterministic ICU collation is fine, and the index still answers.
DROP INDEX cg_ok;
ALTER TABLE cg ALTER COLUMN s TYPE text COLLATE "und-x-icu";
CREATE INDEX cg_icu ON cg USING tre (s);
SET enable_seqscan = off;
SELECT s FROM cg WHERE s %~~ tre_pattern('Café', 0) ORDER BY s;
RESET enable_seqscan;

DROP TABLE cg;
DROP COLLATION cg_ci;
