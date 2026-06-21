-- pg_tre 2.0.2 -> 2.0.3-dev upgrade.
--
-- This stub is filled in as catalog-changing features land in
-- the dev cycle.  At release time, run the audit from
-- RELEASING.md to confirm every new CREATE FUNCTION /
-- OPERATOR / OPERATOR CLASS / TYPE / CAST / ALTER OPERATOR
-- FAMILY in sql/pg_tre--2.0.3-dev.sql has a matching statement
-- here, and every removed/renamed object has a matching
-- DROP / ALTER.

-- 2.0.3: page-kind histogram diagnostic.
CREATE FUNCTION tre_page_kind_histogram(regclass)
    RETURNS TABLE(page_kind text, n_pages bigint,
                  used_bytes bigint, used_pct numeric)
    AS 'MODULE_PATHNAME', 'tre_page_kind_histogram'
    LANGUAGE C STRICT STABLE PARALLEL SAFE
    ROWS 12;

COMMENT ON FUNCTION tre_page_kind_histogram(regclass) IS
    'Per-page-kind tally (count, used bytes, fill %) over every block of '
    'the index.  Read-only diagnostic for localizing where an index''s '
    'size lives -- which page kind dominates and whether those pages are '
    'full or mostly empty.';
