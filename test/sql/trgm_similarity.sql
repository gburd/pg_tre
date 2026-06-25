-- Phase A (A2): pg_trgm-compatible trigram similarity.
-- Verifies tre_trgm_similarity / distance / threshold-op semantics.  As
-- of 3.0.0 pg_tre does NOT create the bare %, <-> operators (they collide
-- with pg_trgm); the same behavior is exercised via the underlying
-- functions tre_trgm_sim_op(a,b) (the old % procedure) and
-- tre_trgm_distance(a,b) (the old <-> procedure).  Expected values are
-- pinned to pg_trgm's known outputs.
CREATE EXTENSION IF NOT EXISTS pg_tre;

-- similarity matches pg_trgm similarity() exactly.
SELECT round(tre_trgm_similarity('foo','foobar')::numeric, 6) AS foo_foobar;      -- 0.375
SELECT round(tre_trgm_similarity('hello','hallo')::numeric, 6) AS hello_hallo;    -- 0.333333
SELECT round(tre_trgm_similarity('abc','xyz')::numeric, 6)  AS abc_xyz;           -- 0.000000
SELECT round(tre_trgm_similarity('cat','category')::numeric, 6) AS cat_category;  -- 0.300000
SELECT round(tre_trgm_similarity('aaa','aaa')::numeric, 6)   AS self;             -- 1.000000

-- distance = 1 - similarity.
SELECT round(tre_trgm_distance('foo','foobar')::numeric, 6) AS dist_foo_foobar;   -- 0.625000

-- the threshold op (old %) honors pg_tre.similarity_threshold.
SET pg_tre.similarity_threshold = 0.3;
SELECT tre_trgm_sim_op('foo', 'foobar') AS at_0_375_thr_0_3;   -- true (0.375 >= 0.3)
SET pg_tre.similarity_threshold = 0.5;
SELECT tre_trgm_sim_op('foo', 'foobar') AS at_0_375_thr_0_5;   -- false (0.375 < 0.5)
RESET pg_tre.similarity_threshold;

-- distance is orderable: closest first.
WITH v(s) AS (VALUES ('foobar'),('food'),('xyz'),('fool'))
SELECT s FROM v ORDER BY tre_trgm_distance(s, 'foo') LIMIT 3;
