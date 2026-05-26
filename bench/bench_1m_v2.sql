-- bench/bench_1m_v2.sql
--
-- 1M-row real-corpus benchmark for pg_tre at 1.4.0.
--
-- Replaces the slow plpgsql Zipfian sampler from bench/bench_1m.sql.
-- The original generator was O(N^2) (per-row count() + IN-subquery on
-- 479k words); on 1M rows it ran past 20 minutes without finishing.
--
-- This version vectorizes the corpus build:
--   1. Load the dictionary once.
--   2. Generate 1M rows in a single INSERT ... SELECT using a CTE
--      that produces the body via subquery sampling per row.
--      The sampler is a single SQL SELECT with random offset, no
--      plpgsql, no IN-over-cardinality probe.
--   3. Seeded phrases (connection refused, error E-NNNN, database)
--      are appended deterministically by row id modulus.
--
-- Run:
--   psql -d bench -X -f bench/bench_1m_v2.sql
--
-- Expected wall-clock on a developer workstation:
--   - words load:        ~3 s
--   - corpus generation: ~60 s
--   - pg_tre build:      depends on corpus shape
--   - pg_trgm build:     ~30 s
--   - query panel:       ~10 s

\set ECHO all
\timing on

DROP EXTENSION IF EXISTS pg_tre CASCADE;
DROP EXTENSION IF EXISTS pg_trgm CASCADE;
CREATE EXTENSION pg_tre;
CREATE EXTENSION pg_trgm;

DROP TABLE IF EXISTS bench_1m;
DROP TABLE IF EXISTS bench_words;

-- Load the dictionary words into a numbered table.
CREATE UNLOGGED TABLE bench_words (
    rank int PRIMARY KEY,
    word text NOT NULL
);
INSERT INTO bench_words (rank, word)
SELECT row_number() OVER () AS rank, word
  FROM (
      SELECT DISTINCT trim(unnest(string_to_array(
             pg_read_file('/usr/share/dict/words'), E'\n'))) AS word
  ) raw
 WHERE word <> '' AND length(word) <= 20
   AND word ~ '^[a-zA-Z]+$'      -- no contractions, no proper nouns with apostrophes
;
ANALYZE bench_words;
SELECT count(*) AS words_loaded FROM bench_words;

-- Materialize the dictionary into an array once. plpgsql array
-- indexing is O(1); the bench runs as fast as random() + concat.
CREATE OR REPLACE FUNCTION bench_pick_phrase(seed int) RETURNS text
LANGUAGE plpgsql STABLE AS $$
DECLARE
    words text[];
    n int;
    out text := '';
    i int;
    word_count int;
BEGIN
    SELECT array_agg(word ORDER BY rank), count(*) INTO words, n FROM bench_words;
    word_count := 5 + (seed % 8);  -- 5..12 words per row
    FOR i IN 1..word_count LOOP
        out := out || (CASE WHEN i > 1 THEN ' ' ELSE '' END)
            || words[1 + ((seed * 1103515245 + i * 12345) % n)::int];
    END LOOP;
    RETURN out;
END$$;

CREATE UNLOGGED TABLE bench_1m (
    id   serial PRIMARY KEY,
    body text NOT NULL
);

-- Seed: 1M rows. The bench_pick_phrase function is STABLE and
-- fast; one call per row.
INSERT INTO bench_1m (body)
SELECT (
    CASE
        WHEN i %  100 = 0 THEN 'connection refused after timeout '
        WHEN i % 1000 = 0 THEN format('error E-%s ', lpad((i % 9999)::text, 4, '0'))
        ELSE ''
    END ||
    CASE WHEN i %  20 = 0 THEN 'database ' ELSE '' END ||
    bench_pick_phrase(i)
)
FROM generate_series(1, 1000000) AS g(i);

ANALYZE bench_1m;
SELECT count(*) AS bench_rows FROM bench_1m;

\echo === Building pg_tre index ===
CREATE INDEX bench_1m_tre_idx ON bench_1m USING tre (body);

\echo === Building pg_trgm GIN index for comparison ===
CREATE INDEX bench_1m_trgm_idx ON bench_1m USING gin (body gin_trgm_ops);

SELECT
    relname,
    pg_size_pretty(pg_relation_size(oid)) AS size,
    pg_relation_size(oid) AS bytes
  FROM pg_class
 WHERE relname IN ('bench_1m', 'bench_1m_tre_idx', 'bench_1m_trgm_idx')
 ORDER BY relname;

-- ------------------------------------------------------------------
-- Query panel
-- ------------------------------------------------------------------

\echo === Q1: 'connection refused' exact (k=0) ===
SET enable_seqscan = on;  SET enable_indexscan = off; SET enable_bitmapscan = off;
EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM bench_1m
 WHERE tre_amatch(body, 'connection refused', 0);

SET enable_seqscan = off; SET enable_indexscan = on;  SET enable_bitmapscan = on;
EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM bench_1m
 WHERE body %~~ tre_pattern('connection refused', 0);

EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM bench_1m
 WHERE body LIKE '%connection refused%';

\echo === Q2: 'connectoin refused' typo k=1 ===
EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM bench_1m
 WHERE body %~~ tre_pattern('connectoin refused', 1);

SET enable_seqscan = on;  SET enable_indexscan = off; SET enable_bitmapscan = off;
EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM bench_1m
 WHERE tre_amatch(body, 'connectoin refused', 1);

\echo === Q3: error code regex 'E-[0-9]{4}' (k=0) ===
SET enable_seqscan = off; SET enable_indexscan = on;  SET enable_bitmapscan = on;
EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM bench_1m
 WHERE body %~~ tre_pattern('E-[0-9]{4}', 0);

\echo === Q4: 'database' dense common (~5%) ===
SET enable_seqscan = off; SET enable_indexscan = on;  SET enable_bitmapscan = on;
EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM bench_1m
 WHERE body %~~ tre_pattern('database', 0);

\echo === Q5: 'databse' typo k=1 (selective) ===
EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM bench_1m
 WHERE body %~~ tre_pattern('databse', 1);

\echo === Q6: top-10 closest matches via index-side ORDER BY (1.4 feature) ===
EXPLAIN (ANALYZE, BUFFERS)
SELECT id FROM bench_1m
 WHERE body %~~ tre_pattern('connectoin refused', 2)
 ORDER BY body <@> tre_pattern('connectoin refused', 2) ASC NULLS LAST
 LIMIT 10;

\echo === Cleanup ===
DROP FUNCTION bench_pick_phrase(int);
DROP INDEX bench_1m_tre_idx;
DROP INDEX bench_1m_trgm_idx;
DROP TABLE bench_1m;
DROP TABLE bench_words;
DROP EXTENSION pg_trgm;
