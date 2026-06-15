-- Phase A (A1): LIKE / ILIKE / ~ / ~* / = index acceleration.
-- Each operator must (a) be answerable by a pg_tre index and
-- (b) return exactly the same rows as a forced seq scan.  ILIKE
-- and ~* fall back to full recheck (the index is case-sensitive)
-- but must still be correct.
CREATE EXTENSION IF NOT EXISTS pg_tre;

CREATE TABLE la_t(id serial primary key, body text);
INSERT INTO la_t(body)
SELECT md5(g::text) || (CASE WHEN g % 10 = 0 THEN ' needle_xyz' ELSE '' END)
FROM generate_series(1, 3000) g;
CREATE INDEX la_idx ON la_t USING tre (body);
ANALYZE la_t;

-- Index-driven counts.
SET enable_seqscan = off;
SELECT count(*) AS like_idx   FROM la_t WHERE body LIKE '%needle_xyz%';
SELECT count(*) AS regex_idx  FROM la_t WHERE body ~ 'needle_x.z';
SELECT count(*) AS ilike_idx  FROM la_t WHERE body ILIKE '%NEEDLE_XYZ%';
SELECT count(*) AS iregex_idx FROM la_t WHERE body ~* 'NEEDLE';

-- Seq-scan ground truth (must equal the above).
SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS like_seq   FROM la_t WHERE body LIKE '%needle_xyz%';
SELECT count(*) AS regex_seq  FROM la_t WHERE body ~ 'needle_x.z';
SELECT count(*) AS ilike_seq  FROM la_t WHERE body ILIKE '%NEEDLE_XYZ%';
SELECT count(*) AS iregex_seq FROM la_t WHERE body ~* 'NEEDLE';

-- Sub-trigram pattern falls back gracefully (no crash, correct count).
SET enable_seqscan = off;
SELECT count(*) AS short_idx FROM la_t WHERE body LIKE '%xy%';
SET enable_seqscan = on; SET enable_indexscan = off; SET enable_bitmapscan = off;
SELECT count(*) AS short_seq FROM la_t WHERE body LIKE '%xy%';

DROP TABLE la_t;
