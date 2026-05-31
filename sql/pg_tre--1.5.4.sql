-- pg_tre 1.5.4 -- native index AM for approximate regex matching.
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
-- Debug UDFs (Phase 3)
-- ---------------------------------------------------------------------

CREATE FUNCTION tre_parse_debug(text)
    RETURNS text
    AS 'MODULE_PATHNAME', 'tre_parse_debug'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION tre_extract_debug(text)
    RETURNS text
    AS 'MODULE_PATHNAME', 'tre_extract_debug'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION tre_extract_debug(text, int)
    RETURNS text
    AS 'MODULE_PATHNAME', 'tre_extract_debug_k'
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
-- tre_pattern type
-- ---------------------------------------------------------------------

CREATE TYPE tre_pattern;

CREATE FUNCTION tre_pattern_in(cstring)
    RETURNS tre_pattern
    AS 'MODULE_PATHNAME', 'tre_pattern_in'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION tre_pattern_out(tre_pattern)
    RETURNS cstring
    AS 'MODULE_PATHNAME', 'tre_pattern_out'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION tre_pattern_recv(internal)
    RETURNS tre_pattern
    AS 'MODULE_PATHNAME', 'tre_pattern_recv'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION tre_pattern_send(tre_pattern)
    RETURNS bytea
    AS 'MODULE_PATHNAME', 'tre_pattern_send'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE TYPE tre_pattern (
    INPUT = tre_pattern_in,
    OUTPUT = tre_pattern_out,
    RECEIVE = tre_pattern_recv,
    SEND = tre_pattern_send,
    STORAGE = extended
);

-- Constructor functions

CREATE FUNCTION tre_pattern(text)
    RETURNS tre_pattern
    AS 'MODULE_PATHNAME', 'tre_pattern_make'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION tre_pattern(text, int4)
    RETURNS tre_pattern
    AS 'MODULE_PATHNAME', 'tre_pattern_make_k'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION tre_pattern(text, int4, int4, int4, int4)
    RETURNS tre_pattern
    AS 'MODULE_PATHNAME', 'tre_pattern_make_full'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- ---------------------------------------------------------------------
-- Operators
-- ---------------------------------------------------------------------

CREATE FUNCTION tre_match_scalar(text, tre_pattern)
    RETURNS bool
    AS 'MODULE_PATHNAME', 'tre_match_scalar'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION tre_pattern_sel(internal, oid, internal, int4)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'tre_pattern_sel'
    LANGUAGE C STRICT;

CREATE OPERATOR %~~ (
    LEFTARG = text,
    RIGHTARG = tre_pattern,
    FUNCTION = tre_match_scalar,
    RESTRICT = tre_pattern_sel
);

COMMENT ON OPERATOR %~~ (text, tre_pattern) IS
    'approximate regex match (pg_tre)';

-- ---------------------------------------------------------------------
-- Opclass for text columns.
--
-- Strategy 1: text %~~ tre_pattern  (approximate regex match)
-- ---------------------------------------------------------------------

DROP OPERATOR CLASS IF EXISTS tre_text_ops USING tre CASCADE;

CREATE OPERATOR CLASS tre_text_ops
    DEFAULT FOR TYPE text USING tre AS
    OPERATOR 1 %~~ (text, tre_pattern),
    STORAGE text;

-- ---------------------------------------------------------------
-- 1.3.0 additions: similarity / distance ranking helpers.
-- ---------------------------------------------------------------

CREATE FUNCTION tre_similarity(text, text, int4)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'pg_tre_similarity'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION tre_similarity(text, text, int4) IS
    'Approximate-match similarity score in [0.0, 1.0]. '
    'Returns 1 - cost/max(len(input), len(pattern)) if the input '
    'matches the pattern within max_cost edits, else 0.0.';

CREATE FUNCTION tre_similarity(text, tre_pattern)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'pg_tre_similarity_pattern'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION tre_similarity(text, tre_pattern) IS
    'Approximate-match similarity against a strongly-typed pattern. '
    'See tre_similarity(text, text, int4) for semantics.';

CREATE FUNCTION tre_distance(text, text, int4)
    RETURNS int4
    AS 'MODULE_PATHNAME', 'pg_tre_distance'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION tre_distance(text, text, int4) IS
    'Approximate-match edit distance (insertions + deletions + '
    'substitutions, each weighted 1).  NULL if the input does not '
    'match the pattern within max_cost edits.';

CREATE FUNCTION tre_distance(text, tre_pattern)
    RETURNS int4
    AS 'MODULE_PATHNAME', 'pg_tre_distance_pattern'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION tre_distance(text, tre_pattern) IS
    'Approximate-match edit distance against a strongly-typed pattern. '
    'See tre_distance(text, text, int4) for semantics.';

CREATE OPERATOR <@> (
    LEFTARG    = text,
    RIGHTARG   = tre_pattern,
    PROCEDURE  = tre_distance
    -- No commutator: tre_pattern is non-commutative (left side
    -- is the input, right side is the pattern).
);

COMMENT ON OPERATOR <@> (text, tre_pattern) IS
    'Approximate-match edit distance.  Returns the integer cost '
    'of the best alignment of input against pattern, or NULL if '
    'no match exists within the pattern''s max_cost.  Use ORDER '
    'BY (text <@> pattern) ASC LIMIT N to rank candidates by '
    'similarity.';

-- ---------------------------------------------------------------
-- 1.5.0 addition: ORDER BY <@> support in the index AM.
--
-- Strategy 2 = ordering operator returning int4 (edit distance).
-- The integer_ops opfamily is the btree int4 ordering family;
-- ASC NULLS LAST yields closest matches first.
-- Combined with amcanorderbyop=true in the AM handler, this lets
--   ORDER BY body <@> tre_pattern('foo', 2) ASC LIMIT N
-- be answered directly by the index without an executor Sort.
-- ---------------------------------------------------------------

ALTER OPERATOR FAMILY tre_text_ops USING tre ADD
    OPERATOR 2 <@> (text, tre_pattern) FOR ORDER BY integer_ops;

-- ---------------------------------------------------------------
-- 1.5.0 addition: in-place format-upgrade infrastructure.
--
-- Per-page format_version is tracked in the opaque trailer of each
-- pg_tre page; the meta page tracks the minimum across all pages.
-- Future on-disk format bumps can be applied lazily without REINDEX:
--
--    SELECT pg_tre_upgrade_index('my_idx');
--
-- See doc/onpage_format.md.
-- ---------------------------------------------------------------

CREATE FUNCTION pg_tre_upgrade_index(regclass)
    RETURNS void
    AS 'MODULE_PATHNAME', 'pg_tre_upgrade_index'
    LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

COMMENT ON FUNCTION pg_tre_upgrade_index(regclass) IS
    'Walk every block of a pg_tre index, rewriting any page below the '
    'current on-disk format version to the latest format in place.  '
    'Per-page exclusive lock is held only for the duration of each '
    'rewrite; readers and writers proceed concurrently.  WAL-logged '
    'as XLOG_PTRE_PAGE_FORMAT_UPGRADE (full-page image, one record '
    'per rewritten page).  Updates the meta page''s '
    'min_page_format_version after a complete sweep.';

CREATE FUNCTION pg_tre_index_format_status(regclass)
    RETURNS TABLE(format_version int4, page_count bigint)
    AS 'MODULE_PATHNAME', 'pg_tre_index_format_status'
    LANGUAGE C STRICT STABLE PARALLEL SAFE
    ROWS 4;

COMMENT ON FUNCTION pg_tre_index_format_status(regclass) IS
    'Return one row per distinct per-page format_version observed in '
    'the index, with a count of pages at that version.  Walks all '
    'blocks under shared locks; safe to run concurrently with other '
    'workloads.';

CREATE FUNCTION pg_tre_index_min_format_version(regclass)
    RETURNS int4
    AS 'MODULE_PATHNAME', 'pg_tre_index_min_format_version'
    LANGUAGE C STRICT STABLE PARALLEL SAFE;

COMMENT ON FUNCTION pg_tre_index_min_format_version(regclass) IS
    'Return the meta page''s min_page_format_version field, i.e. the '
    'minimum per-page format_version known to exist in the index.  '
    'Cheap O(1); for the authoritative full-walk view use '
    'pg_tre_index_format_status().';
