-- test/sql/order_by.sql
-- Regression test for the index-side ORDER BY <@> path added in
-- 1.4.0-dev (amcanorderbyop=true + amgettuple).
--
-- The pg_tre access method now answers
--     SELECT ... WHERE body %~~ pat ORDER BY body <@> pat LIMIT N
-- by streaming TIDs in ascending distance order from the index AM
-- itself, with no executor-side Sort.

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS ob_t CASCADE;
CREATE TABLE ob_t (id serial PRIMARY KEY, body text);

-- A controlled mix:  rows with exact-match 'foobar' (dist 0), one-edit
-- variants 'fobar'/'foobaar'/'fooba'/'fooobar' (dist 1), two-edit
-- variants (dist 2), and a large pool of bystander rows that contain
-- entirely unrelated trigrams (no match within k=2 budget).
INSERT INTO ob_t (body) VALUES
    ('exact foobar match alpha'),         -- dist 0
    ('exact foobar match beta'),          -- dist 0
    ('exact foobar match gamma'),         -- dist 0
    ('one-edit fobar variant'),           -- dist 1 (deletion)
    ('one-edit foobaar variant'),         -- dist 1 (insertion)
    ('one-edit fooba variant'),           -- dist 1 (deletion at end)
    ('one-edit fooxbar variant'),         -- dist 1 (insertion middle)
    ('two-edit fxxbar variant'),          -- dist 2
    ('two-edit fboar variant'),           -- dist 2
    ('two-edit foxbat variant');          -- dist 2

-- Filler rows that share no useful trigrams with 'foobar'.
INSERT INTO ob_t (body)
SELECT 'unrelated bystander row ' || g || ' xyzzy'
FROM generate_series(1, 200) AS g;

-- Suppress the version-varying build-progress NOTICEs (trigram/posting
-- counts) so the expected file is stable across PG majors.
SET client_min_messages = warning;
CREATE INDEX ob_idx ON ob_t USING tre (body);
ANALYZE ob_t;

-- ----------------------------------------------------------------
-- 1. Plan shape: index-driven ordered scan, no executor Sort.
--    With k=0 the trigram extraction is non-trivial so the index
--    drives the scan (no always_true fallback).  Version-robust:
--    probe the plan tree for an Index Scan carrying an Order By and
--    the absence of any Sort node, via a single text token (no exact
--    plan-shape dump, which differs across PG majors).
-- ----------------------------------------------------------------
SET enable_seqscan = off;
SET enable_bitmapscan = off;

CREATE FUNCTION ob_index_ordered(q text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE r record; plan jsonb; txt text;
BEGIN
  FOR r IN EXECUTE 'EXPLAIN (FORMAT JSON) ' || q LOOP
    plan := r."QUERY PLAN";
  END LOOP;
  txt := plan::text;
  RETURN txt ILIKE '%Index Scan%'
     AND txt ILIKE '%Order By%'
     AND txt NOT ILIKE '%"Node Type": "Sort"%';
END $$;
SELECT CASE WHEN ob_index_ordered($$
         SELECT id FROM ob_t
         WHERE body %~~ tre_pattern('foobar', 0)
         ORDER BY body <@> tre_pattern('foobar', 0)
         LIMIT 5$$)
            THEN 'index_ordered_no_sort' ELSE 'sort_or_seq' END AS plan_shape;
DROP FUNCTION ob_index_ordered(text);

-- ----------------------------------------------------------------
-- 2. Result correctness: rows in ascending distance order.
--    With k=0 we only see exact-substring matches (dist 0 rows).
--    Tie-break on id for deterministic output.
-- ----------------------------------------------------------------
SELECT id, body, body <@> tre_pattern('foobar', 0) AS dist
FROM ob_t
WHERE body %~~ tre_pattern('foobar', 0)
ORDER BY body <@> tre_pattern('foobar', 0) ASC, id ASC
LIMIT 10;

-- ----------------------------------------------------------------
-- 3. Cross-check against seq-scan + executor sort: same top-N rows.
-- ----------------------------------------------------------------
SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;

SELECT id, body, body <@> tre_pattern('foobar', 0) AS dist
FROM ob_t
WHERE body %~~ tre_pattern('foobar', 0)
ORDER BY body <@> tre_pattern('foobar', 0) ASC, id ASC
LIMIT 10;

-- ----------------------------------------------------------------
-- 4. The two paths must agree on the top-5 ID list.
-- ----------------------------------------------------------------
SET enable_seqscan = off;
SET enable_indexscan = on;
SET enable_bitmapscan = off;

WITH idx AS (
    SELECT id FROM ob_t
    WHERE body %~~ tre_pattern('foobar', 0)
    ORDER BY body <@> tre_pattern('foobar', 0) ASC, id ASC
    LIMIT 5
), seq AS (
    SELECT id FROM ob_t
    WHERE body %~~ tre_pattern('foobar', 0)
    ORDER BY tre_distance(body, tre_pattern('foobar', 0)) ASC, id ASC
    LIMIT 5
)
SELECT (SELECT array_agg(id ORDER BY id) FROM idx) =
       (SELECT array_agg(id ORDER BY id) FROM seq)
       AS top5_matches;

-- Cleanup
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
DROP TABLE ob_t CASCADE;
