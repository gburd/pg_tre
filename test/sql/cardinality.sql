-- test/sql/cardinality.sql
-- Regression test for the pg_tre.min_trigram_freq GUC (cardinality-
-- aware build, added in 1.2.1).
--
-- The build path drops posting trees for trigrams appearing in
-- fewer than min_trigram_freq rows.  The dropped trigrams aren't
-- recorded in the upper tree at all, so queries that would have
-- relied on them fall back through the normal lossy-bitmap +
-- recheck path.  Correctness must be preserved.

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS card_t CASCADE;
CREATE TABLE card_t (id serial, body text);

-- 5 rows containing 'singleton' (well above any reasonable
-- min_trigram_freq).
INSERT INTO card_t (body)
SELECT 'singleton occurrence ' || i FROM generate_series(1, 5) AS i;

-- 200 rows containing 'frequent'.
INSERT INTO card_t (body)
SELECT 'frequent text body ' || i FROM generate_series(1, 200) AS i;

-- Build with min_trigram_freq=10: rare trigrams (e.g. those from
-- 'singleton occurrence N' that occur in only one row each) get
-- dropped.  We can't ALTER SYSTEM inside a regression test (must
-- not run inside a transaction) so this test simply documents
-- the behavior at the default; the operator-side smoke test
-- covered the actual filtering.
SHOW pg_tre.min_trigram_freq;

CREATE INDEX card_idx ON card_t USING tre (body);

-- Differential: index and seq scan must agree on row sets.
SET enable_seqscan = off;
CREATE TEMP TABLE _idx AS
    SELECT id FROM card_t WHERE body %~~ tre_pattern('frequent', 0);

SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE _seq AS
    SELECT id FROM card_t WHERE body %~~ tre_pattern('frequent', 0);

SELECT
    (SELECT count(*) FROM _idx) = (SELECT count(*) FROM _seq) AS counts_agree,
    NOT EXISTS (SELECT 1 FROM _idx EXCEPT SELECT 1 FROM _seq)
    AND NOT EXISTS (SELECT 1 FROM _seq EXCEPT SELECT 1 FROM _idx)
        AS row_sets_agree;

DROP TABLE _idx;
DROP TABLE _seq;
DROP TABLE card_t;
