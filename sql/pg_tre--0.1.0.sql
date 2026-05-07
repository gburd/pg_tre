-- pg_tre: Approximate regex matching using TRE

-- Core approximate match: returns true if input approximately matches
-- the regex pattern within the given max_cost edit distance.
CREATE FUNCTION tre_amatch(text, text, int4)
    RETURNS bool
    AS 'MODULE_PATHNAME', 'pg_tre_amatch'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Returns the cost of the best approximate match, or NULL if no match
-- is found within max_cost.
CREATE FUNCTION tre_amatch_cost(text, text, int4)
    RETURNS int4
    AS 'MODULE_PATHNAME', 'pg_tre_amatch_cost'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Approximate match with explicit per-operation costs.
-- Arguments: input, pattern, max_cost, cost_ins, cost_del, cost_subst
CREATE FUNCTION tre_amatch(text, text, int4, int4, int4, int4)
    RETURNS bool
    AS 'MODULE_PATHNAME', 'pg_tre_amatch_with_costs'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Full detail: returns a row with cost breakdown and match positions.
-- Returns zero rows if no match is found.
CREATE FUNCTION tre_amatch_detail(text, text, int4)
    RETURNS TABLE(cost int4, num_ins int4, num_del int4, num_subst int4,
                  match_start int4, match_end int4)
    AS 'MODULE_PATHNAME', 'pg_tre_amatch_detail'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE
    ROWS 1;

-- Returns the pg_tre and TRE library version string.
CREATE FUNCTION tre_version()
    RETURNS text
    AS 'MODULE_PATHNAME', 'pg_tre_version'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
