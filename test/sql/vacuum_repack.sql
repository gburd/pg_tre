-- VACUUM of a posting whose repacked sparsemap would exceed its original
-- inline slot must NOT error -- it migrates the posting out-of-line.
--
-- Removing a TID from the interior of an RLE run can split it, so a
-- sparsemap with fewer members can serialize larger than the original.
-- Before 2.0.4 this ERROR'd ("inline vacuum repack overflow") and aborted
-- VACUUM; 2.0.4 kept the original blob (no removal); 2.1.0 MIGRATES the
-- survivors to a dedicated posting leaf so the dead TIDs are actually
-- reclaimed (self-healing).  Builds are payload-free as of 3.0.0 (no
-- inline slack), so we build, DELETE + VACUUM twice, and assert the
-- index keeps agreeing with a sequential scan.  Single-token robust style.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS vr_t;
CREATE TABLE vr_t (id serial PRIMARY KEY, body text);
INSERT INTO vr_t(body)
SELECT 'noise' || g
    || (CASE WHEN (g * 2654435761) % 3 = 0 THEN ' midtoken' ELSE '' END)
FROM generate_series(1, 5000) g;

SET pg_tre.coalesce_enable = on;
CREATE INDEX vr_idx ON vr_t USING tre (body);
SET pg_tre.coalesce_enable = off;

-- delete a scattered chunk + VACUUM (the path that used to overflow),
-- then confirm index == seq-scan.
DELETE FROM vr_t WHERE id % 5 = 0;
VACUUM vr_t;
SELECT CASE WHEN
  (SELECT count(*) FROM vr_t WHERE body %~~ tre_pattern('midtoken', 0)) =
  (SELECT count(*) FROM vr_t WHERE body LIKE '%midtoken%')
  THEN 'vacuum1_ok' ELSE 'vacuum1_bad' END AS r1;

-- a second, heavier churn + VACUUM.
DELETE FROM vr_t WHERE id % 3 = 0;
VACUUM vr_t;
SELECT CASE WHEN
  (SELECT count(*) FROM vr_t WHERE body %~~ tre_pattern('midtoken', 0)) =
  (SELECT count(*) FROM vr_t WHERE body LIKE '%midtoken%')
  THEN 'vacuum2_ok' ELSE 'vacuum2_bad' END AS r2;

DROP TABLE vr_t;
