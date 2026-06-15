-- pg_tre 1.10.1 -> 1.11.0 upgrade.
--
-- Phase A (A2 remainder): pg_trgm-compatible word_similarity.
--
-- word_similarity(a,b) is the greatest trigram Jaccard similarity of
-- a against any contiguous trigram extent of b (asymmetric).
-- strict_word_similarity pins extents to word boundaries.  Operators
-- mirror pg_trgm: <%, <<->, <<%, <<<->.  No on-disk format change.

CREATE FUNCTION tre_word_similarity(text, text)
    RETURNS float4 AS 'MODULE_PATHNAME', 'pg_tre_word_similarity'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION tre_strict_word_similarity(text, text)
    RETURNS float4 AS 'MODULE_PATHNAME', 'pg_tre_strict_word_similarity'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION tre_word_sim_op(text, text)
    RETURNS bool AS 'MODULE_PATHNAME', 'pg_tre_word_sim_op'
    LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE FUNCTION tre_word_dist_op(text, text)
    RETURNS float4 AS 'MODULE_PATHNAME', 'pg_tre_word_dist_op'
    LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE FUNCTION tre_strict_word_sim_op(text, text)
    RETURNS bool AS 'MODULE_PATHNAME', 'pg_tre_strict_word_sim_op'
    LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE FUNCTION tre_strict_word_dist_op(text, text)
    RETURNS float4 AS 'MODULE_PATHNAME', 'pg_tre_strict_word_dist_op'
    LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE OPERATOR <% (
    LEFTARG = text, RIGHTARG = text, PROCEDURE = tre_word_sim_op,
    RESTRICT = contsel, JOIN = contjoinsel
);

CREATE OPERATOR <<-> (
    LEFTARG = text, RIGHTARG = text, PROCEDURE = tre_word_dist_op
);

CREATE OPERATOR <<% (
    LEFTARG = text, RIGHTARG = text, PROCEDURE = tre_strict_word_sim_op,
    RESTRICT = contsel, JOIN = contjoinsel
);

CREATE OPERATOR <<<-> (
    LEFTARG = text, RIGHTARG = text, PROCEDURE = tre_strict_word_dist_op
);
