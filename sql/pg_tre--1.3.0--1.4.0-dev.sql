-- pg_tre upgrade 1.3.0 -> 1.4.0-dev
--
-- Adds index-side ORDER BY <@> support:
--   * Strategy 2 of tre_text_ops becomes the ordering operator
--     <@> (text, tre_pattern), returning int4 edit distance.
--   * Combined with amcanorderbyop=true in the AM handler and a new
--     amgettuple implementation, the planner can answer
--       SELECT ... WHERE body %~~ pat ORDER BY body <@> pat LIMIT N
--     directly from the index, in ascending distance order, without
--     materialising the full result set into a Sort node.
--
-- The on-disk index format is unchanged.  Existing indexes do not
-- need REINDEX; only the catalog is touched.

ALTER OPERATOR FAMILY tre_text_ops USING tre ADD
    OPERATOR 2 <@> (text, tre_pattern) FOR ORDER BY integer_ops;

-- 1.4.0-dev: in-place format-upgrade infrastructure.  See
-- doc/onpage_format.md for the design.  These functions are no-ops on
-- 1.3.x indexes since v3 and v4 share a byte layout, but the
-- machinery is in place for the next on-disk format change.

CREATE FUNCTION pg_tre_upgrade_index(regclass)
    RETURNS void
    AS 'MODULE_PATHNAME', 'pg_tre_upgrade_index'
    LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION pg_tre_index_format_status(regclass)
    RETURNS TABLE(format_version int4, page_count bigint)
    AS 'MODULE_PATHNAME', 'pg_tre_index_format_status'
    LANGUAGE C STRICT STABLE PARALLEL SAFE
    ROWS 4;

CREATE FUNCTION pg_tre_index_min_format_version(regclass)
    RETURNS int4
    AS 'MODULE_PATHNAME', 'pg_tre_index_min_format_version'
    LANGUAGE C STRICT STABLE PARALLEL SAFE;
