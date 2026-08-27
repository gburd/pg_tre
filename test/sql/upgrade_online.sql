-- test/sql/upgrade_online.sql
-- Regression test for online (no-REINDEX) format upgrade machinery.
--
-- pg_tre_upgrade_index() walks every page and rewrites any below the
-- latest on-disk format in place, under a per-page exclusive lock, while
-- readers/writers proceed.  A fresh index is already at the latest format
-- (v9), so the upgrade is a no-op walk that must succeed cleanly and leave
-- the index answering identically.  The key regression this guards is the
-- page-format dispatch covering the whole supported range [MIN, LATEST]
-- (a gap there previously errored "page format upgrade from vN not
-- implemented" for an intermediate version).

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS up_t CASCADE;
CREATE TABLE up_t (id serial, body text);
INSERT INTO up_t (body)
SELECT 'line ' || i || ' ' || md5(i::text) FROM generate_series(1, 2000) i;

CREATE INDEX up_idx ON up_t USING tre (body);

-- Baseline query result.
SET enable_seqscan = off;
SELECT count(*) AS before_upgrade FROM up_t WHERE body %~~ tre_pattern('line', 0);
RESET enable_seqscan;

-- Every page reports the latest format version for a freshly-built index.
SELECT format_version, page_count > 0 AS has_pages
FROM pg_tre_index_format_status('up_idx')
ORDER BY format_version;

-- The min format version is the latest (nothing older present).
SELECT pg_tre_index_min_format_version('up_idx') AS min_fmt;

-- Run the in-place upgrade walk: must succeed with no error even though
-- there is nothing to rewrite (exercises the per-page dispatch for every
-- page kind at the current version).
SELECT pg_tre_upgrade_index('up_idx');

-- Result is unchanged after the upgrade walk.
SET enable_seqscan = off;
SELECT count(*) AS after_upgrade FROM up_t WHERE body %~~ tre_pattern('line', 0);
RESET enable_seqscan;

DROP TABLE up_t CASCADE;
