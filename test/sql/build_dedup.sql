-- Per-row (trigram,tid) de-dup: heavily repetitive text emits one
-- tuple per DISTINCT trigram per row, not per occurrence.  Verify
-- (a) the estimate reflects the small distinct count, and
-- (b) correctness is preserved (index matches == seq-scan matches).
CREATE EXTENSION IF NOT EXISTS pg_tre;

CREATE TABLE dd_t(id serial primary key, body text);
INSERT INTO dd_t(body)
SELECT repeat('needle ', 200) || (CASE WHEN g % 100 = 0 THEN 'rarehay' ELSE '' END)
FROM generate_series(1, 300) g;
ANALYZE dd_t;

-- repeat('needle ',200) is 1400 bytes/row but a handful of distinct
-- trigrams: est_trigrams/row must be small (dedup working), not ~1400.
SELECT CASE WHEN est_trigrams < 50 * est_rows THEN 'dedup_collapsed'
            ELSE 'dedup_not_working' END AS dedup_result
FROM tre_estimate_index_build('dd_t'::regclass, 2);

-- Correctness preserved: index matches must equal seq-scan matches.
CREATE INDEX dd_idx ON dd_t USING tre (body);
SELECT CASE WHEN
  (SELECT count(*) FROM dd_t WHERE body %~~ tre_pattern('needle', 0)) =
  (SELECT count(*) FROM dd_t WHERE body LIKE '%needle%')
  THEN 'needle_correct' ELSE 'needle_wrong' END AS needle_result;
SELECT CASE WHEN
  (SELECT count(*) FROM dd_t WHERE body %~~ tre_pattern('rarehay', 0)) =
  (SELECT count(*) FROM dd_t WHERE body LIKE '%rarehay%')
  THEN 'rare_correct' ELSE 'rare_wrong' END AS rare_result;

DROP TABLE dd_t;
