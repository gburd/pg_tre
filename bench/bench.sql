-- pg_tre benchmark driver (simple form).
-- Generates a 10k-row corpus and times exact + approximate regex
-- against pg_tre and pg_trgm.
--
-- Usage: psql -d bench_db -f bench/bench.sql

DROP TABLE IF EXISTS docs_tre;
DROP TABLE IF EXISTS docs_trgm;

CREATE EXTENSION IF NOT EXISTS pg_tre;
CREATE EXTENSION IF NOT EXISTS pg_trgm;

CREATE TABLE docs_tre  (id serial PRIMARY KEY, body text);
CREATE TABLE docs_trgm (id serial PRIMARY KEY, body text);

-- Realistic fixture: 10k short sentences with vocabulary + unique md5 tag.
INSERT INTO docs_tre(body)
SELECT format('user_%s tried to %s the %s but encountered error %s code %s',
              i, w1, w2, i*7, md5(i::text)::text)
FROM generate_series(1, 10000) i,
     LATERAL (SELECT (ARRAY['login','query','update','insert','delete',
                            'configure','restart','optimize'])
                      [1 + (i % 8)] AS w1) x,
     LATERAL (SELECT (ARRAY['database','server','index','table','connection',
                            'session','cache','buffer'])
                      [1 + ((i*13) % 8)] AS w2) y;

INSERT INTO docs_trgm(body) SELECT body FROM docs_tre;

\timing on

\echo ========= BUILD =========
SET client_min_messages=warning;
CREATE INDEX docs_tre_idx  ON docs_tre  USING tre (body);
CREATE INDEX docs_trgm_idx ON docs_trgm USING gin (body gin_trgm_ops);
RESET client_min_messages;

SELECT pg_size_pretty(pg_relation_size('docs_tre_idx'))  AS pg_tre_idx_size;
SELECT pg_size_pretty(pg_relation_size('docs_trgm_idx')) AS pg_trgm_idx_size;

ANALYZE docs_tre;
ANALYZE docs_trgm;

\echo
\echo ========= SELECTIVE EXACT REGEX (md5 prefix, 1-row match) =========
\echo -- Warm-up
SELECT count(*) FROM docs_tre  WHERE body %~~ tre_pattern('e62d29', 0);
SELECT count(*) FROM docs_trgm WHERE body ~ 'e62d29';
\echo -- Timed
SELECT count(*) FROM docs_tre  WHERE body %~~ tre_pattern('e62d29', 0);
SELECT count(*) FROM docs_tre  WHERE body %~~ tre_pattern('e62d29', 0);
SELECT count(*) FROM docs_trgm WHERE body ~ 'e62d29';
SELECT count(*) FROM docs_trgm WHERE body ~ 'e62d29';

\echo
\echo ========= APPROXIMATE K=1 (22-row match) =========
SELECT count(*) FROM docs_tre WHERE body %~~ tre_pattern('e62d2', 1);
SELECT count(*) FROM docs_tre WHERE body %~~ tre_pattern('e62d2', 1);

\echo
\echo ========= APPROXIMATE K=1 vs SEQ-SCAN BASELINE =========
SET enable_indexscan=off;
SET enable_bitmapscan=off;
SELECT count(*) FROM docs_tre WHERE tre_amatch(body, 'e62d2', 1);
SET enable_indexscan=on;
SET enable_bitmapscan=on;

\echo
\echo ========= EXPLAIN =========
EXPLAIN (COSTS OFF) SELECT id FROM docs_tre  WHERE body %~~ tre_pattern('e62d29', 0);
EXPLAIN (COSTS OFF) SELECT id FROM docs_trgm WHERE body ~ 'e62d29';
