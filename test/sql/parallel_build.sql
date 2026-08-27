-- test/sql/parallel_build.sql
-- Regression test for parallel CREATE INDEX (amcanbuildparallel = true,
-- pg_tre.enable_parallel_build on by default).
--
-- The parallelizable phase is the heap scan + trigram extraction +
-- coordinated tuplesort; the leader merges every participant's partial
-- run and serially builds the posting trees, upper tree, and range tier.
-- This test confirms:
--
--   1. A parallel build succeeds and produces a valid index.
--   2. The parallel-built index returns exactly the same rows as a
--      seq scan (the correctness gate: no tuple is lost or duplicated
--      across the participant split).
--   3. A serial build of the same data answers identically.

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS pbuild_t CASCADE;
CREATE TABLE pbuild_t (id serial, body text);

INSERT INTO pbuild_t (body)
SELECT md5(i::text) || ' error' || (i % 100) || ' token' || (i % 13)
FROM generate_series(1, 20000) AS i;

-- Force the planner to assign build workers regardless of table size.
SET max_parallel_maintenance_workers = 4;
SET max_parallel_workers = 8;
SET min_parallel_table_scan_size = 0;
SET maintenance_work_mem = '32MB';
SET pg_tre.enable_parallel_build = on;
SET client_min_messages = warning;   -- suppress per-build NOTICEs (worker count varies)

CREATE INDEX pbuild_par ON pbuild_t USING tre (body);

-- Index must be valid.
SELECT indisvalid, indisready FROM pg_index
WHERE indexrelid = 'pbuild_par'::regclass;

-- Differential vs seq scan across several patterns.
SET enable_seqscan = off;
SELECT count(*) AS idx_exact FROM pbuild_t WHERE body %~~ tre_pattern('error42', 0);
SELECT count(*) AS idx_token FROM pbuild_t WHERE body %~~ tre_pattern('token7', 0);
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS seq_exact FROM pbuild_t WHERE body %~~ tre_pattern('error42', 0);
SELECT count(*) AS seq_token FROM pbuild_t WHERE body %~~ tre_pattern('token7', 0);

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;

-- A serial build of the same data must answer identically.
SET pg_tre.enable_parallel_build = off;
CREATE INDEX pbuild_ser ON pbuild_t USING tre (body);

SET enable_seqscan = off;
SELECT count(*) AS ser_exact FROM pbuild_t WHERE body %~~ tre_pattern('error42', 0);
RESET enable_seqscan;

DROP TABLE pbuild_t CASCADE;
