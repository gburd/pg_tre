-- pg_tre 1.0.0 -- native index AM for approximate regex matching.
--
-- Phase 0 scope: registers the `tre` access method, the legacy UDFs
-- inherited from 0.1.0, and the handler function.  The opclass is
-- declared so CREATE INDEX ... USING tre succeeds syntactically; scan
-- and insert paths are wired in by later phases and will raise a
-- clear "not yet implemented" error until then.
--
-- Do not run ALTER EXTENSION ... UPDATE on a production index; the
-- on-disk format is stabilized at 1.0.0.

-- ---------------------------------------------------------------------
-- Legacy UDFs (unchanged from 0.1.0)
-- ---------------------------------------------------------------------

CREATE FUNCTION tre_amatch(text, text, int4)
    RETURNS bool
    AS 'MODULE_PATHNAME', 'pg_tre_amatch'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION tre_amatch_cost(text, text, int4)
    RETURNS int4
    AS 'MODULE_PATHNAME', 'pg_tre_amatch_cost'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION tre_amatch(text, text, int4, int4, int4, int4)
    RETURNS bool
    AS 'MODULE_PATHNAME', 'pg_tre_amatch_with_costs'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION tre_amatch_detail(text, text, int4)
    RETURNS TABLE(cost int4, num_ins int4, num_del int4, num_subst int4,
                  match_start int4, match_end int4)
    AS 'MODULE_PATHNAME', 'pg_tre_amatch_detail'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE
    ROWS 1;

CREATE FUNCTION tre_version()
    RETURNS text
    AS 'MODULE_PATHNAME', 'pg_tre_version'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- ---------------------------------------------------------------------
-- Access method registration
-- ---------------------------------------------------------------------

CREATE FUNCTION tre_handler(internal)
    RETURNS index_am_handler
    AS 'MODULE_PATHNAME', 'tre_handler'
    LANGUAGE C;

CREATE ACCESS METHOD tre
    TYPE INDEX
    HANDLER tre_handler;

COMMENT ON ACCESS METHOD tre IS
    'approximate-regex index (pg_tre)';

-- ---------------------------------------------------------------------
-- Opclass for text columns.
--
-- Strategy 1: text %~~ tre_pattern  (approximate regex match)
--
-- Phase 0 registers only the shape of the opclass so CREATE INDEX
-- parses cleanly; the %~~ operator and tre_pattern type are added in
-- Phase 3.
-- ---------------------------------------------------------------------

CREATE OPERATOR CLASS tre_text_ops
    DEFAULT FOR TYPE text USING tre AS
    STORAGE text;
