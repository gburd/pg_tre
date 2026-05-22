-- pg_tre 1.1.1 -> 1.2.0-dev upgrade.
--
-- Adds similarity / distance functions for ranking approximate
-- matches by edit distance.
--
-- Usage:
--   SELECT body, tre_distance(body, 'fox', 2) AS dist
--   FROM t
--   WHERE body %~~ tre_pattern('fox', 2)
--   ORDER BY dist ASC
--   LIMIT 10;

-- Similarity score in [0.0, 1.0] derived from edit distance.
-- 1.0 means exact match; 0.0 means no match within max_cost or the
-- input was entirely rewritten.  Matches pg_trgm's similarity()
-- semantics (higher = more similar).
CREATE FUNCTION tre_similarity(text, text, int4)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'pg_tre_similarity'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION tre_similarity(text, text, int4) IS
    'Approximate-match similarity score in [0.0, 1.0]. '
    'Returns 1 - cost/max(len(input), len(pattern)) if the input '
    'matches the pattern within max_cost edits, else 0.0.';

-- Same against tre_pattern (carries its own max_cost).
CREATE FUNCTION tre_similarity(text, tre_pattern)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'pg_tre_similarity_pattern'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION tre_similarity(text, tre_pattern) IS
    'Approximate-match similarity against a strongly-typed pattern. '
    'See tre_similarity(text, text, int4) for semantics.';

-- Raw edit distance.  NULL if no match within max_cost.
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

-- The <@> operator ("eyeball") is the ergonomic spelling for
-- ranking by approximate-match edit distance.  Returns the
-- distance as int4; NULL if no match within the pattern's
-- max_cost (NULLs sort last by default in ASC ordering, so
-- the top-N closest matches come first).
--
-- Inspired by pg_textsearch's <@> for BM25 ranking and
-- pg_trgm's <-> for trigram distance.  Unlike pg_textsearch,
-- we don't need to invert the sign: the natural distance is
-- already ASC-friendly (smaller = more similar).
--
-- Idiom:
--   SELECT body, body <@> tre_pattern('fox', 2) AS dist
--   FROM t
--   WHERE body %~~ tre_pattern('fox', 2)
--   ORDER BY dist ASC NULLS LAST
--   LIMIT 10;
--
-- The %~~ in the WHERE clause uses the tre index to narrow
-- candidates; the ORDER BY sorts those candidates in memory.
-- Index-side ORDER BY support is planned for v2.0.
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
