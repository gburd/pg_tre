-- Phase A (A2 remainder): word_similarity / strict_word_similarity.
-- Values must equal pg_trgm's (verified against the live extension):
--   word_similarity('cat','category')                = 0.75
--   word_similarity('word','two words here')         = 0.8
--   strict_word_similarity('word','two words here')  = 0.5714286
-- The pg_tre operators (% / <-> / word ones) intentionally collide
-- with pg_trgm's by name; the two extensions are never co-installed.
CREATE EXTENSION IF NOT EXISTS pg_tre;

SELECT tre_word_similarity('cat', 'category')            AS ws_cat_category;
SELECT tre_word_similarity('cat', 'a category dog')      AS ws_cat_extent;
SELECT tre_word_similarity('word', 'two words here')     AS ws_word;
SELECT tre_word_similarity('foo', 'the foobar baz')      AS ws_foo;
SELECT tre_word_similarity('xyz', 'abcdefg')             AS ws_nomatch;
SELECT tre_word_similarity('', 'empty')                  AS ws_empty;

SELECT tre_strict_word_similarity('word', 'two words here') AS sws_word;
SELECT tre_strict_word_similarity('cat', 'category')        AS sws_cat;

-- Operators.
SELECT 'cat' <%  'a category dog'  AS op_word_sim;        -- true (>= 0.3)
SELECT 'xyz' <%  'abcdefg'         AS op_word_sim_no;     -- false
SELECT round((('cat' <<-> 'a category dog')::numeric), 6) AS op_word_dist;
SELECT 'word' <<% 'two words here' AS op_strict_sim;      -- 0.5714 >= 0.3 -> true
SELECT round((('word' <<<-> 'two words here')::numeric), 6) AS op_strict_dist;

-- Threshold GUC is honored by the % family.
SET pg_tre.similarity_threshold = 0.9;
SELECT 'cat' <% 'a category dog' AS op_high_threshold;    -- 0.75 < 0.9 -> false
RESET pg_tre.similarity_threshold;
