-- bench/bench_1m.sql
--
-- Real-corpus benchmark for pg_tre at 1M rows.
--
-- Closes the 'bench at scale' v1.0.0-final blocker.  Replaces the
-- 10K-row demo in bench/bench.sql with a fixture large enough to
-- shake out posting-tree behaviour at production sizes.
--
-- Corpus shape (synthetic, Zipfian, 1M rows):
--   - Each row's body is a 5-12 word phrase sampled from
--     /usr/share/dict/words with a power-law bias toward the
--     start of the file (pseudo-Zipfian: random()^4).
--   - 1% of rows are seeded with the literal "connection refused"
--     somewhere in the body (target for k=0 / k=1 patterns).
--   - 0.1% are seeded with "error E-NNNN" for a 4-digit numeric
--     code (target for character-class regexes).
--   - 5% are seeded with "database" (a dense common token, used
--     to stress-test posting trees that today cap at one leaf).
--
-- Run:
--   psql -d bench -X -f bench/bench_1m.sql
--
-- Expected wall-clock on a 2024 laptop (single core, no NUMA):
--   - words load:        ~5 s
--   - corpus generation: ~30 s
--   - pg_tre build:      ~60 s   (or ERROR if multi-leaf is missing)
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

-- One-shot: load /usr/share/dict/words into a numbered table so
-- per-row sampling can pick by index.
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
 WHERE word <> '' AND length(word) <= 20;
ANALYZE bench_words;
SELECT count(*) AS words_loaded FROM bench_words;

-- Helper: sample N words with Zipfian bias (lower rank == more frequent).
CREATE OR REPLACE FUNCTION bench_pick_words(n int) RETURNS text
LANGUAGE plpgsql VOLATILE STRICT AS $$
DECLARE
    total int;
    out text;
BEGIN
    SELECT count(*) INTO total FROM bench_words;
    SELECT coalesce(string_agg(word, ' '), 'placeholder')
      INTO out
      FROM (
          SELECT word FROM bench_words
           WHERE rank IN (
              SELECT (1 + (random()^4 * (total - 1)))::int
                FROM generate_series(1, n)
           )
      ) s;
    RETURN out;
END$$;

CREATE UNLOGGED TABLE bench_1m (
    id   serial PRIMARY KEY,
    body text NOT NULL
);

-- Seed: 1M rows
INSERT INTO bench_1m (body)
SELECT (
    -- Seeded phrases (small fraction of rows)
    CASE
        WHEN i %  100 = 0 THEN 'connection refused after timeout '
        WHEN i % 1000 = 0 THEN format('error E-%s ', lpad((i % 9999)::text, 4, '0'))
        ELSE ''
    END ||
    CASE WHEN i %  20 = 0 THEN 'database ' ELSE '' END ||
    -- Per-row Zipfian sample of 5-12 dictionary words
    bench_pick_words(5 + (random() * 7)::int)
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
    pg_size_pretty(pg_relation_size(oid)) AS size
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

SET enable_seqscan = on;  SET enable_indexscan = off; SET enable_bitmapscan = off;
EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM bench_1m
 WHERE tre_amatch(body, 'E-[0-9]{4}', 0);

\echo === Q4: 'database' dense common (~5%) ===
SET enable_seqscan = off; SET enable_indexscan = on;  SET enable_bitmapscan = on;
EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM bench_1m
 WHERE body %~~ tre_pattern('database', 0);

SET enable_seqscan = on;  SET enable_indexscan = off; SET enable_bitmapscan = off;
EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM bench_1m
 WHERE tre_amatch(body, 'database', 0);

\echo === Q5: 'databse' typo k=1 ===
SET enable_seqscan = off; SET enable_indexscan = on;  SET enable_bitmapscan = on;
EXPLAIN (ANALYZE, BUFFERS) SELECT count(*) FROM bench_1m
 WHERE body %~~ tre_pattern('databse', 1);

\echo === Cleanup ===
DROP FUNCTION bench_pick_words(int);
DROP INDEX bench_1m_tre_idx;
DROP INDEX bench_1m_trgm_idx;
DROP TABLE bench_1m;
DROP TABLE bench_words;
DROP EXTENSION pg_trgm;
