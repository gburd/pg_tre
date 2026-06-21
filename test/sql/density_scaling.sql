-- Density is controllable and bounded across N with the per-tuple
-- positional payload disabled (v2.0.2 field report: a few-trigram corpus
-- blew up ~50x past 10k rows because that payload -- positions + bloom --
-- is stored uncompressed at ~22 B/TID and could not be turned off per
-- build, being PGC_SIGHUP).  As of 2.0.3 pg_tre.tuple_bloom_enable is
-- PGC_USERSET, so an operator can build a dense index when the lossy
-- positional pre-filter is not needed.  This test builds the same small-
-- alphabet corpus at 10k and 30k with the payload OFF and coalescing ON,
-- and asserts 30k bytes/row stays within 4x of 10k bytes/row (it was 50x).
-- (Build-only: VACUUM of a payload-off inline posting has a separate
-- repack issue tracked for a follow-up; not exercised here.)
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

SET pg_tre.tuple_bloom_enable = off;
SET pg_tre.coalesce_enable = on;
CREATE INDEX dscale_10k_idx ON dscale_10k USING tre (body);
CREATE INDEX dscale_30k_idx ON dscale_30k USING tre (body);
SET pg_tre.coalesce_enable = off;
SET pg_tre.tuple_bloom_enable = on;

SELECT CASE WHEN
    (pg_relation_size('dscale_30k_idx') / 30000)
      <= (pg_relation_size('dscale_10k_idx') / 10000) * 4
  THEN 'density_bounded' ELSE 'density_blowup' END AS scaling;

DROP TABLE dscale_10k;
DROP TABLE dscale_30k;
