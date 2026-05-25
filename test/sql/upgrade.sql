-- test/sql/upgrade.sql
-- Regression test for in-place format-upgrade infrastructure
-- (1.4.0-dev).
--
-- Exercises:
--   1. pg_tre_index_format_status reports a single row at the
--      current LATEST format on a freshly-built index.
--   2. pg_tre_index_min_format_version returns LATEST.
--   3. pg_tre_upgrade_index runs cleanly (no-op upgrade in this
--      release because v3 and v4 share a byte layout).
--   4. After running the upgrade function, scans still return
--      correct results -- no page corruption, no LSN regressions.
--   5. pg_tre_upgrade_index is idempotent.
--   6. The functions error out on non-pg_tre relations.

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS upg_t CASCADE;
CREATE TABLE upg_t (id serial PRIMARY KEY, body text);

INSERT INTO upg_t (body)
SELECT 'document number ' || i || ' contains ' ||
       md5(i::text) || ' as a marker'
FROM generate_series(1, 500) AS i;

CREATE INDEX upg_idx ON upg_t USING tre (body);

-- (1) Format status: every page should be at the current LATEST.
SELECT format_version,
       page_count > 0 AS has_pages
FROM pg_tre_index_format_status('upg_idx'::regclass)
ORDER BY format_version;

-- (2) Min format version matches LATEST exactly.
SELECT pg_tre_index_min_format_version('upg_idx'::regclass) AS min_fmt;

-- (3) Run pg_tre_upgrade_index.  Must succeed cleanly.
SELECT pg_tre_upgrade_index('upg_idx'::regclass);

-- (4) Scan still returns correct results.
SET enable_seqscan = off;
SET enable_bitmapscan = on;
SELECT count(*) AS hits
FROM upg_t
WHERE body %~~ tre_pattern('document', 0);

-- (5) Idempotent: running the upgrade a second time still works
--     and the format-status output is unchanged.
SELECT pg_tre_upgrade_index('upg_idx'::regclass);

SELECT format_version,
       page_count > 0 AS has_pages
FROM pg_tre_index_format_status('upg_idx'::regclass)
ORDER BY format_version;

SELECT pg_tre_index_min_format_version('upg_idx'::regclass) AS min_fmt_after;

-- (6) Error path: non-pg_tre relation rejected.
DO $$
BEGIN
    PERFORM pg_tre_upgrade_index('upg_t'::regclass);
    RAISE EXCEPTION 'expected error on heap table';
EXCEPTION WHEN wrong_object_type THEN
    RAISE NOTICE 'rejected heap table as expected';
END $$;

DO $$
BEGIN
    PERFORM pg_tre_index_format_status('upg_t_pkey'::regclass);
    RAISE EXCEPTION 'expected error on btree index';
EXCEPTION WHEN wrong_object_type THEN
    RAISE NOTICE 'rejected btree index as expected';
END $$;

DROP TABLE upg_t CASCADE;
