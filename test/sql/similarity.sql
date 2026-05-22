-- test/sql/similarity.sql
-- Regression test for tre_similarity, tre_distance, and the <@>
-- operator added in 1.2.0-dev.

CREATE EXTENSION IF NOT EXISTS pg_tre;

-- ----------------------------------------------------------------
-- Basic distance / similarity on text input (no index involved).
-- ----------------------------------------------------------------

-- Exact match: distance 0, similarity 1.
SELECT tre_distance('fox', 'fox', 2);
SELECT tre_similarity('fox', 'fox', 2);

-- One substitution: distance 1.
SELECT tre_distance('fix', 'fox', 2);
SELECT tre_distance('fix'::text, tre_pattern('fox', 2));

-- One deletion + one insertion against pattern of length 3 in
-- input of length 5: distance = 2.
SELECT tre_distance('boxtr', 'fox', 2);

-- No match within budget: NULL.
--
-- Note: 'xyzzy' against 'fox' with k=2 actually MATCHES because TRE
-- finds the substring 'x' inside 'xyzzy' and reaches it from 'fox'
-- via two deletions (delete 'f', delete 'o', keep 'x'), cost=2.
-- TRE's approximate match looks for any substring; it doesn't
-- require the whole input to match the whole pattern.
SELECT tre_distance('zzz', 'fox', 2);  -- truly no shared chars
SELECT tre_distance('zzz'::text, tre_pattern('fox', 2));

-- 'foxtrot' (len=7), 'fox' (len=3), max=7, cost=0 (exact-substring
-- match), so similarity = 1 - 0/7 = 1.0.
SELECT tre_similarity('foxtrot'::text, 'fox'::text, 2);

-- 'faxtrat' against 'fox' with k=2: best alignment is one
-- substitution (a->o); cost=1, max_len=7, sim=1-1/7=0.857...
SELECT round(tre_similarity('faxtrat'::text, 'fox'::text, 2)::numeric, 3);

-- A truly-disjoint input returns sim=0.0 within k=0.
SELECT tre_similarity('zzz'::text, 'fox'::text, 0);

-- ----------------------------------------------------------------
-- The <@> operator: ORDER BY distance against an indexed table.
-- ----------------------------------------------------------------

DROP TABLE IF EXISTS sim_t CASCADE;
CREATE TABLE sim_t (id serial, body text);
INSERT INTO sim_t (body) VALUES
    ('the quick brown fox'),     -- exact-substring 'fox', dist 0
    ('the quick brwn fx'),       -- 'fx' is 1 deletion from 'fox'
    ('the quick brown fix'),     -- 'fix' is 1 substitution from 'fox'
    ('a slow gray cat'),         -- only matches via partial 'o'
    ('foxtrot here'),            -- exact substring 'fox', dist 0
    ('boxtrot here'),            -- 1 sub
    ('zzz nothing here');        -- no shared trigram
CREATE INDEX sim_idx ON sim_t USING tre (body);

-- ORDER BY <@> ASC NULLS LAST returns closest matches first.  We
-- sort by id as a tiebreaker for determinism.
SET enable_seqscan = off;
SELECT body, body <@> tre_pattern('fox', 2) AS dist
FROM sim_t
WHERE body %~~ tre_pattern('fox', 2)
ORDER BY dist ASC NULLS LAST, id ASC;

-- Without the index (force seq scan): same result.
SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT body, body <@> tre_pattern('fox', 2) AS dist
FROM sim_t
WHERE body <@> tre_pattern('fox', 2) IS NOT NULL
ORDER BY dist ASC NULLS LAST, id ASC;

-- Using <@> in WHERE clause works (filters NULL = no-match).
SELECT count(*) FROM sim_t
WHERE body <@> tre_pattern('fox', 2) IS NOT NULL;

-- The <@> result is consistent with tre_distance().
SELECT
    body,
    body <@> tre_pattern('fox', 2) = tre_distance(body, tre_pattern('fox', 2))
        AS consistent
FROM sim_t
ORDER BY id;

-- Cleanup.
SET enable_indexscan = on;
SET enable_bitmapscan = on;
DROP TABLE sim_t CASCADE;
