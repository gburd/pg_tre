-- test/sql/pending_limit.sql
-- 4.5.0: pending_list_limit merges the pending list on INSERT; fastupdate
-- = off is refused; pg_tre_index_check compares the index with a
-- sequential scan and catches an index that under-returns.

\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_tre;
DROP TABLE IF EXISTS pl CASCADE;
RESET client_min_messages;

CREATE TABLE pl (id int, s text);
INSERT INTO pl SELECT g, 'seed row ' || g || ' quick brown fox' FROM generate_series(1, 500) g;
SET client_min_messages = warning;
CREATE INDEX pl_lim ON pl USING tre (s) WITH (pending_list_limit = 64);
RESET client_min_messages;

-- The list is drained by an INSERT once it passes 64 kB (2730 entries of
-- 24 bytes), so it stays below that however many rows arrive (5000 rows,
-- ~55 entries each).  With no limit it holds all of them until VACUUM.
INSERT INTO pl SELECT g, 'row ' || g || ' quick brawn fox ' || md5(g::text)
  FROM generate_series(501, 5500) g;
SELECT pg_tre_index_pending('pl_lim') <= 2730 + 60 AS bounded_by_limit;

-- No limit (0): nothing merges until VACUUM.
SET client_min_messages = warning;
CREATE INDEX pl_nolim ON pl USING tre (s) WITH (pending_list_limit = 0);
RESET client_min_messages;
INSERT INTO pl SELECT g, 'row ' || g || ' quick brawn fox ' || md5(g::text)
  FROM generate_series(5501, 10500) g;
SELECT pg_tre_index_pending('pl_nolim') > 100000 AS grows_without_limit;
VACUUM pl;
SELECT pg_tre_index_pending('pl_nolim') AS after_vacuum;
DROP INDEX pl_nolim;

-- The index agrees with a sequential scan.
SET client_min_messages = warning;
SELECT count(*) >= 50 AS checks, count(*) FILTER (WHERE ok IS NOT TRUE) AS failing
  FROM pg_tre_index_check('pl_lim', 10);
RESET client_min_messages;

-- The GUC applies when the storage parameter is unset, and the parameter
-- can be changed online.
ALTER INDEX pl_lim RESET (pending_list_limit);
SET pg_tre.pending_list_limit = 64;
INSERT INTO pl SELECT g, 'guc row ' || g || ' lazy dog' FROM generate_series(10501, 15500) g;
SELECT pg_tre_index_pending('pl_lim') <= 2730 + 60 AS bounded_by_guc;
RESET pg_tre.pending_list_limit;
ALTER INDEX pl_lim SET (pending_list_limit = 128);

-- fastupdate = off is refused where it is set.
CREATE INDEX pl_bad ON pl USING tre (s) WITH (fastupdate = off);
ALTER INDEX pl_lim SET (fastupdate = off);
SET pg_tre.fastupdate = off;
SET pg_tre.fastupdate = on;
SET pg_tre.pending_list_limit = -1;
ALTER INDEX pl_lim SET (pending_list_limit = -2);

-- pg_tre_index_check catches an index that misses rows: rows inserted
-- while the index is not maintained (indisready = false) are in the heap
-- but not in the index.
CREATE TABLE plx (id int, s text);
INSERT INTO plx SELECT g, 'alpha beta gamma ' || (g % 50) FROM generate_series(1, 200) g;
SET client_min_messages = warning;
CREATE INDEX plx_idx ON plx USING tre (s);
UPDATE pg_index SET indisready = false WHERE indexrelid = 'plx_idx'::regclass;
RESET client_min_messages;
INSERT INTO plx SELECT g, 'alpha beta gamma ' || (g % 50) FROM generate_series(201, 4000) g;
SET client_min_messages = error;
UPDATE pg_index SET indisready = true WHERE indexrelid = 'plx_idx'::regclass;
SELECT count(*) FILTER (WHERE ok = false) > 0 AS catches_missing_rows
  FROM pg_tre_index_check('plx_idx', 10);
RESET client_min_messages;

-- Errors: not a tre index; an expression index.
CREATE INDEX plx_btree ON plx (id);
SELECT * FROM pg_tre_index_check('plx_btree');
SET client_min_messages = warning;
CREATE INDEX plx_expr ON plx USING tre (lower(s));
RESET client_min_messages;
SELECT * FROM pg_tre_index_check('plx_expr');

DROP TABLE pl, plx;

-- pg_tre_index_check pins its search_path (4.5.1): functions an untrusted
-- schema puts ahead of pg_catalog are never called (4.5.0 called these 40
-- times as the invoking user).
CREATE SCHEMA ic_evil;
CREATE TABLE ic_calls (note text);
CREATE FUNCTION ic_evil.upper(text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN INSERT INTO public.ic_calls VALUES ('upper'); RETURN pg_catalog.upper($1); END $$;
CREATE FUNCTION ic_evil.substr(text, int, int) RETURNS text LANGUAGE plpgsql AS $$
BEGIN INSERT INTO public.ic_calls VALUES ('substr'); RETURN pg_catalog.substr($1, $2, $3); END $$;
CREATE TABLE ic_t (s text);
INSERT INTO ic_t SELECT 'hello world ' || g FROM generate_series(1, 300) g;
SET client_min_messages = warning;
CREATE INDEX ic_t_idx ON ic_t USING tre (s);
SET search_path = ic_evil, pg_catalog, public;
SELECT count(*) > 0 AS checks FROM pg_tre_index_check('ic_t_idx', 5);
RESET search_path;
RESET client_min_messages;
SELECT count(*) AS hijacked_calls FROM ic_calls;
DROP TABLE ic_t, ic_calls;
DROP SCHEMA ic_evil CASCADE;
