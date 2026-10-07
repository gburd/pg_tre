-- test/sql/encoding_nonutf8.sql
--
-- pg_tre decodes characters in the DATABASE ENCODING (pg_mblen et al.),
-- both in the trigram tokenizer and -- via patches/tre-mbdecoder.patch --
-- inside TRE.  Before 4.3.0 the tokenizer hard-coded UTF-8, so any
-- non-ASCII text in a LATIN1 or EUC_JP database failed with "invalid
-- UTF-8 sequence".
--
-- Runs in fresh LATIN1 and EUC_JP databases (template0).  Literals are
-- written in UTF-8 and converted by the client encoding, so this file is
-- itself plain UTF-8.

\set VERBOSITY terse
\set QUIET on
\set home :DBNAME
SET client_min_messages = warning;
SET client_encoding = 'UTF8';
DROP DATABASE IF EXISTS pgtre_latin1;
DROP DATABASE IF EXISTS pgtre_eucjp;
CREATE DATABASE pgtre_latin1 TEMPLATE template0 ENCODING 'LATIN1' LOCALE 'C';
CREATE DATABASE pgtre_eucjp  TEMPLATE template0 ENCODING 'EUC_JP' LOCALE 'C';

\c pgtre_latin1
SET client_encoding = 'UTF8';
SET client_min_messages = warning;
CREATE EXTENSION pg_tre;
CREATE TABLE t (id int, s text);
INSERT INTO t VALUES (1, 'café society'), (2, 'cafe society'),
                     (3, 'naïve résumé'), (4, 'unrelated');
CREATE INDEX t_tre ON t USING tre (s);
-- Exact and fuzzy, via the index and via a seq scan: identical.
SET enable_seqscan = off;
SELECT 'latin1 idx' AS via, array_agg(id ORDER BY id) AS k0
  FROM t WHERE s %~~ tre_pattern('café', 0);
SELECT 'latin1 idx' AS via, array_agg(id ORDER BY id) AS k1
  FROM t WHERE s %~~ tre_pattern('café', 1);
SELECT 'latin1 idx' AS via, array_agg(id ORDER BY id) AS resume
  FROM t WHERE s %~~ tre_pattern('résumé', 0);
RESET enable_seqscan;
SET enable_indexscan = off; SET enable_bitmapscan = off;
SELECT 'latin1 seq' AS via, array_agg(id ORDER BY id) AS k1
  FROM t WHERE s %~~ tre_pattern('café', 1);
RESET enable_indexscan; RESET enable_bitmapscan;
-- Edits are characters: one each.
SELECT tre_amatch_cost('cafe', 'café', 5) AS cost,
       tre_amatch_cost('naive', 'naïve', 5) AS cost2;
-- Inserts after the build go through aminsert and the pending list.
INSERT INTO t VALUES (5, 'Ça va très bien');
SET enable_seqscan = off;
SELECT array_agg(id) AS pending FROM t WHERE s %~~ tre_pattern('très', 0);
RESET enable_seqscan;

\c pgtre_eucjp
SET client_encoding = 'UTF8';
SET client_min_messages = warning;
CREATE EXTENSION pg_tre;
CREATE TABLE t (id int, s text);
INSERT INTO t VALUES (1, '日本語のテスト'), (2, '日本語のテキスト'),
                     (3, 'ひらがなとカタカナ'), (4, 'ascii only');
CREATE INDEX t_tre ON t USING tre (s);
SET enable_seqscan = off;
SELECT 'eucjp idx' AS via, array_agg(id ORDER BY id) AS k0
  FROM t WHERE s %~~ tre_pattern('日本語のテスト', 0);
SELECT 'eucjp idx' AS via, array_agg(id ORDER BY id) AS k1
  FROM t WHERE s %~~ tre_pattern('日本語のテスト', 1);
RESET enable_seqscan;
SET enable_indexscan = off; SET enable_bitmapscan = off;
SELECT 'eucjp seq' AS via, array_agg(id ORDER BY id) AS k1
  FROM t WHERE s %~~ tre_pattern('日本語のテスト', 1);
RESET enable_indexscan; RESET enable_bitmapscan;
SELECT tre_amatch_cost('日本語のテキスト', '日本語のテスト', 5) AS cost;
-- Match offsets are byte offsets in the database encoding (2 bytes/kana).
SELECT match_start, match_end FROM tre_amatch_detail('xxテストyy', 'テスト', 0);

\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_latin1;
DROP DATABASE pgtre_eucjp;
