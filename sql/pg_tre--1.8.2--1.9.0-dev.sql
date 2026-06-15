-- pg_tre 1.8.2 -> 1.9.0 upgrade.
--
-- Phase A (A2): pg_trgm-compatible trigram-set similarity, so a
-- user can drop pg_trgm and keep %, <->, and word-similarity with
-- the same numeric semantics.  These are cheap, stateless scalar
-- functions over pg_trgm's trigram model -- NOT the pg_tre index's
-- internal trigrams, and NOT edit distance (that remains <@>).
--
-- No on-disk format change; no REINDEX.

-- similarity(a, b) in [0,1]: |A ∩ B| / |A ∪ B| over trigram sets.
CREATE FUNCTION tre_trgm_similarity(text, text)
    RETURNS float4
    AS 'MODULE_PATHNAME', 'pg_tre_trgm_similarity'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION tre_trgm_similarity(text, text) IS
    'Trigram-set Jaccard similarity in [0,1], matching pg_trgm '
    'similarity().  Cheap and stateless; distinct from edit-distance '
    'tre_similarity / <@>.';

-- distance = 1 - similarity (pg_trgm <->).
CREATE FUNCTION tre_trgm_distance(text, text)
    RETURNS float4
    AS 'MODULE_PATHNAME', 'pg_tre_trgm_distance'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION tre_trgm_distance(text, text) IS
    'Trigram-set distance (1 - similarity), matching pg_trgm <->.';

-- %-threshold test: similarity(a,b) >= pg_tre.similarity_threshold.
CREATE FUNCTION tre_trgm_sim_op(text, text)
    RETURNS bool
    AS 'MODULE_PATHNAME', 'pg_tre_trgm_sim_op'
    LANGUAGE C STRICT STABLE PARALLEL SAFE;

COMMENT ON FUNCTION tre_trgm_sim_op(text, text) IS
    'True when tre_trgm_similarity(a,b) >= pg_tre.similarity_threshold. '
    'Backs the %% operator; STABLE because it reads the GUC.';

-- % operator (pg_trgm-compatible similarity threshold test).
-- Commutative: similarity is symmetric.
CREATE OPERATOR % (
    LEFTARG = text,
    RIGHTARG = text,
    PROCEDURE = tre_trgm_sim_op,
    COMMUTATOR = %,
    RESTRICT = contsel,
    JOIN = contjoinsel
);

COMMENT ON OPERATOR % (text, text) IS
    'pg_trgm-compatible trigram similarity threshold match (pg_tre).';

-- <-> operator (pg_trgm-compatible trigram distance, orderable).
-- Commutative: distance is symmetric.
CREATE OPERATOR <-> (
    LEFTARG = text,
    RIGHTARG = text,
    PROCEDURE = tre_trgm_distance,
    COMMUTATOR = <->
);

COMMENT ON OPERATOR <-> (text, text) IS
    'pg_trgm-compatible trigram distance (1 - similarity).  Use '
    'ORDER BY a <-> b for nearest-by-trigram ranking (pg_tre).';
