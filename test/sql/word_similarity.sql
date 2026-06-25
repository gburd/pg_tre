-- Phase A (A2 remainder): word_similarity / strict_word_similarity.
-- Values must equal pg_trgm's (verified against the live extension):
--   word_similarity('cat','category')                = 0.75
--   word_similarity('word','two words here')         = 0.8
--   strict_word_similarity('word','two words here')  = 0.5714286
-- The bare word operators (<% / <<-> / <<% / <<<->) collide with
-- pg_trgm's, so as of 3.0.0 pg_tre does not create them; the same
-- behavior is exercised via the underlying functions
-- tre_word_sim_op / tre_word_dist_op / tre_strict_word_sim_op /
-- tre_strict_word_dist_op (the old operator procedures).
CREATE EXTENSION IF NOT EXISTS pg_tre;

SELECT tre_word_similarity('cat', 'category')            AS ws_cat_category;
SELECT tre_word_similarity('cat', 'a category dog')      AS ws_cat_extent;
SELECT tre_word_similarity('word', 'two words here')     AS ws_word;
SELECT tre_word_similarity('foo', 'the foobar baz')      AS ws_foo;
SELECT tre_word_similarity('xyz', 'abcdefg')             AS ws_nomatch;
SELECT tre_word_similarity('', 'empty')                  AS ws_empty;

SELECT tre_strict_word_similarity('word', 'two words here') AS sws_word;
SELECT tre_strict_word_similarity('cat', 'category')        AS sws_cat;

-- Operator procedures (the old <% / <<-> / <<% / <<<->).
SELECT tre_word_sim_op('cat', 'a category dog')  AS op_word_sim;        -- true (>= 0.3)
SELECT tre_word_sim_op('xyz', 'abcdefg')         AS op_word_sim_no;     -- false
SELECT round((tre_word_dist_op('cat', 'a category dog')::numeric), 6) AS op_word_dist;
SELECT tre_strict_word_sim_op('word', 'two words here') AS op_strict_sim;      -- 0.5714 >= 0.3 -> true
SELECT round((tre_strict_word_dist_op('word', 'two words here')::numeric), 6) AS op_strict_dist;

-- Threshold GUC is honored by the sim-op family.
SET pg_tre.similarity_threshold = 0.9;
SELECT tre_word_sim_op('cat', 'a category dog') AS op_high_threshold;    -- 0.75 < 0.9 -> false
RESET pg_tre.similarity_threshold;
