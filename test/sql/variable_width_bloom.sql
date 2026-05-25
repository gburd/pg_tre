-- variable_width_bloom.sql -- format-v4 per-tuple bloom width selection.
--
-- Format v4 chooses bloom width per row from the row's distinct-trigram
-- count.  The selection function is:
--
--   n <= 8       -> 32 bits
--   n <= 24      -> 64 bits
--   n <= 64      -> 128 bits
--   n <= 200     -> 256 bits
--   n <= 600     -> 512 bits
--   otherwise    -> 1024 bits
--
-- All caps respect pg_tre.bloom_tuple_bits (default 128).  k is chosen
-- via Kirsch-Mitzenmacher as ceil(m * ln 2 / n), clamped to [1, 16].
--
-- This regression test verifies semantic correctness of the variable-
-- width payload across the on-disk posting path.  Tier-3 effectiveness
-- is measured separately.

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS vwb_corpus CASCADE;
CREATE TABLE vwb_corpus (
    id   serial PRIMARY KEY,
    body text
);

-- 200 rows with 'foobar' substring.  Each foobar trigram has 200 TIDs
-- in its posting -> well above the inline budget once payload counts
-- in -> on-disk posting tree -> tier-3 applies.
INSERT INTO vwb_corpus (body)
SELECT 'foobar prefix '   || md5('a' || g) || ' foobar suffix'
FROM generate_series(1, 200) g;

-- Add a few NEEDLE rows so foobar cardinality stays >= the inline
-- threshold; needles are long enough that their unique trigrams (NEE,
-- EED, ...) also produce postings of size >= 200, forcing on-disk.
INSERT INTO vwb_corpus (body)
SELECT 'foobar NEEDLE-' || md5('n' || g) || ' chunk'
FROM generate_series(1, 200) g;

CREATE INDEX vwb_corpus_idx ON vwb_corpus USING tre (body);

-- Sanity: seqscan baseline.
SET enable_seqscan = on;
SET enable_bitmapscan = off;
SET enable_indexscan = off;

SELECT count(*) AS seq_foobar  FROM vwb_corpus
 WHERE body %~~ tre_pattern('foobar', 0);
SELECT count(*) AS seq_needle  FROM vwb_corpus
 WHERE body %~~ tre_pattern('NEEDLE', 0);

-- Bitmap (variable-width tier-3 active).
SET enable_seqscan = off;
SET enable_bitmapscan = on;
SET enable_indexscan = off;

SELECT count(*) AS idx_foobar  FROM vwb_corpus
 WHERE body %~~ tre_pattern('foobar', 0);
SELECT count(*) AS idx_needle  FROM vwb_corpus
 WHERE body %~~ tre_pattern('NEEDLE', 0);

DROP TABLE vwb_corpus CASCADE;
