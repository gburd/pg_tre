-- Customer ask: up-front build sizing precheck.
-- tre_estimate_index_build samples the column and extrapolates the
-- emission count, build temp-disk, and final index size.  Assertions
-- return a single text token to avoid column-width fragility.
CREATE EXTENSION IF NOT EXISTS pg_tre;

CREATE TABLE be_t(id serial primary key, body text);
INSERT INTO be_t(body)
SELECT repeat(md5(g::text), 4) FROM generate_series(1, 500) g;
ANALYZE be_t;

-- One row, sane non-negative numbers.
SELECT CASE WHEN sample_rows > 0 AND est_rows >= sample_rows
             AND est_trigrams > 0 AND est_temp_mb >= 0 AND est_index_mb >= 0
            THEN 'estimate_ok' ELSE 'estimate_bad' END AS result
FROM tre_estimate_index_build('be_t'::regclass, 2);

-- All-null column: zero trigrams and zero sizes, no crash.
CREATE TABLE be_empty(id serial primary key, body text);
INSERT INTO be_empty(body) SELECT NULL FROM generate_series(1, 10);
ANALYZE be_empty;
SELECT CASE WHEN est_trigrams = 0 AND est_temp_mb = 0
            THEN 'empty_ok' ELSE 'empty_bad' END AS result
FROM tre_estimate_index_build('be_empty'::regclass, 2);

DROP TABLE be_t;
DROP TABLE be_empty;
