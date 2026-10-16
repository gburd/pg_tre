-- pg_tre 4.3.1 -> 4.4.0 upgrade.
--
-- This stub is filled in as catalog-changing features land in
-- the dev cycle.  At release time, run the audit from
-- RELEASING.md to confirm every new CREATE FUNCTION /
-- OPERATOR / OPERATOR CLASS / TYPE / CAST / ALTER OPERATOR
-- FAMILY in sql/pg_tre--4.4.0.sql has a matching statement
-- here, and every removed/renamed object has a matching
-- DROP / ALTER.

-- 4.4.0: dense TID packing (format v11).
CREATE FUNCTION pg_tre_index_tid_packing(regclass)
    RETURNS text
    AS 'MODULE_PATHNAME', 'pg_tre_index_tid_packing'
    LANGUAGE C STRICT STABLE PARALLEL SAFE;

COMMENT ON FUNCTION pg_tre_index_tid_packing(regclass) IS
    'How the index packs heap TIDs: ''dense'' (built by pg_tre 4.4.0 or '
    'later on a heap table) or ''legacy'' (built by an earlier version, '
    'or on another table access method).  A legacy index works '
    'unchanged; REINDEX rebuilds it dense, typically 2-2.5x smaller on '
    'long-text columns.';
