-- pg_tre 1.12.0 -> 2.0.0 upgrade.
--
-- Phase B1: on-disk run/level catalog (format v6 -> v7).
--
-- The format change is PURELY ADDITIVE: an existing v6 index is read
-- as a single implicit run, so it keeps working under v7 code with
-- NO REINDEX.  To materialize the v7 catalog fields on the meta page
-- (so the on-disk page is self-describing), run:
--
--   SELECT pg_tre_upgrade_index('your_index'::regclass);
--
-- This is optional in 2.0.0 (read-time normalization handles a v6
-- meta page), and stamps the catalog header without rewriting any
-- posting data.
--
-- New introspection function only; no catalog/operator changes.

CREATE FUNCTION tre_run_catalog_status(regclass)
    RETURNS TABLE(run_id bigint, level int4, n_tuples bigint,
                  n_trigrams bigint, min_hash numeric, max_hash numeric)
    AS 'MODULE_PATHNAME', 'tre_run_catalog_status'
    LANGUAGE C STRICT STABLE PARALLEL SAFE
    ROWS 8;

COMMENT ON FUNCTION tre_run_catalog_status(regclass) IS
    'Introspection over the Phase B1 run/level catalog (format v7): '
    'one row per live run, newest run_id first.  A v6 or default-v7 '
    'index reports a single implicit run rooted at the index roots.';


-- Build sizing precheck (customer ask: estimate temp/final size before
-- committing to a large index build).  Samples up to 2000 rows of the
-- target text column and extrapolates.
CREATE FUNCTION tre_estimate_index_build(regclass, int DEFAULT 1)
    RETURNS TABLE(sample_rows bigint, est_rows bigint,
                  est_trigrams bigint, est_temp_mb bigint,
                  est_index_mb bigint)
    AS 'MODULE_PATHNAME', 'tre_estimate_index_build'
    LANGUAGE C STABLE PARALLEL SAFE;

COMMENT ON FUNCTION tre_estimate_index_build(regclass, int) IS
    'Estimate the temp-disk and final-index size of a TRE index build '
    'on column `attno` of the table, by sampling rows.  Run before '
    'CREATE INDEX on a large column to size build_max_entries_mb and '
    'the temp tablespace up front.  est_temp_mb uses the same '
    'per-tuple cost as the build_max_entries_mb ceiling.';
