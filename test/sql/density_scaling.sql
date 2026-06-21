-- Density must not blow up as row count grows over a FIXED trigram
-- alphabet (v2.0.2 field report: dense at 10k, ~50x blowup at 30k for a
-- corpus with few distinct trigrams, with pg_tre.coalesce_enable ON).
--
-- Root cause was the per-tuple positional payload (positions + bloom),
-- stored uncompressed at ~22 B/TID and on by default via a PGC_SIGHUP
-- GUC that could not be turned off per build.  As of 2.0.3 that payload
-- (pg_tre.tuple_bloom_enable) is OFF by default and PGC_USERSET; it is a
-- lossy pre-filter only (recheck is authoritative).  This test builds
-- the same small-alphabet corpus at 10k and 30k with coalescing ON and
-- asserts 30k bytes/row stays within 4x of 10k bytes/row.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS dscale_10k;
DROP TABLE IF EXISTS dscale_30k;

CREATE TABLE dscale_10k (id serial PRIMARY KEY, body text);
INSERT INTO dscale_10k(body)
SELECT (CASE WHEN g % 2 = 0 THEN 'alpha ' ELSE '' END)
    || (CASE WHEN g % 3 = 0 THEN 'bravo ' ELSE '' END)
    || (CASE WHEN g % 2 = 1 THEN 'charlie ' ELSE '' END)
    || (CASE WHEN g % 5 = 0 THEN 'delta ' ELSE '' END)
    || 'echo foxtrot'
FROM generate_series(1, 10000) g;

CREATE TABLE dscale_30k (id serial PRIMARY KEY, body text);
INSERT INTO dscale_30k(body)
SELECT (CASE WHEN g % 2 = 0 THEN 'alpha ' ELSE '' END)
    || (CASE WHEN g % 3 = 0 THEN 'bravo ' ELSE '' END)
    || (CASE WHEN g % 2 = 1 THEN 'charlie ' ELSE '' END)
    || (CASE WHEN g % 5 = 0 THEN 'delta ' ELSE '' END)
    || 'echo foxtrot'
FROM generate_series(1, 30000) g;

SET pg_tre.coalesce_enable = on;
CREATE INDEX dscale_10k_idx ON dscale_10k USING tre (body);
CREATE INDEX dscale_30k_idx ON dscale_30k USING tre (body);
SET pg_tre.coalesce_enable = off;

SELECT CASE WHEN
    (pg_relation_size('dscale_30k_idx') / 30000)
      <= (pg_relation_size('dscale_10k_idx') / 10000) * 4
  THEN 'density_bounded' ELSE 'density_blowup' END AS scaling;

DROP TABLE dscale_10k;
DROP TABLE dscale_30k;
