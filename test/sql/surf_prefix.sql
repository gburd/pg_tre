-- test/sql/surf_prefix.sql
-- Regression test for the v10 SuRF range-filter prefilter.
--
-- The SuRF indexes an order-preserving trigram key so that an anchored /
-- prefix pattern (^foo, LIKE 'foo%') whose leading trigram key is absent
-- from the whole index can be rejected without scanning the posting tier
-- or the heap.  This test confirms:
--
--   1. A build produces a SuRF (tre_surf_stats reports keys/nodes/pages).
--   2. Anchored queries whose prefix EXISTS return the same rows as a seq
--      scan (SuRF must not drop true matches).
--   3. Anchored queries whose prefix does NOT exist return zero rows
--      (matching the seq-scan ground truth) -- exercising the reject path.
--   4. Non-anchored queries are unaffected (no SuRF range applied).

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS surf_t CASCADE;
CREATE TABLE surf_t (id serial, body text);
INSERT INTO surf_t (body)
SELECT 'error' || i || ' connection refused ' || md5(i::text)
FROM generate_series(1, 3000) i;

CREATE INDEX surf_idx ON surf_t USING tre (body);

-- The index has a SuRF filter with a positive key/node/page count.
SELECT (n_keys > 0) AS has_keys,
       (n_nodes > 0) AS has_nodes,
       (n_pages > 0) AS has_pages
FROM tre_surf_stats('surf_idx');

SET enable_seqscan = off;

-- (2) anchored prefix that EXISTS: index result == seq-scan ground truth.
SELECT count(*) AS idx_exist FROM surf_t WHERE body %~~ tre_pattern('^error', 0);

-- (3) anchored prefix that does NOT exist: SuRF rejects; must be 0 and
--     match the seq-scan ground truth.
SELECT count(*) AS idx_absent FROM surf_t WHERE body %~~ tre_pattern('^zzqqxj', 0);

-- (4) a non-anchored pattern still works (no SuRF range applied).
SELECT count(*) AS idx_unanchored FROM surf_t WHERE body %~~ tre_pattern('refused', 0);

-- (5) REGRESSION: case-insensitive anchored patterns must NOT be rejected by
--     the SuRF prefilter.  The prefix key is derived from the pattern's
--     literal codepoints with NO case folding, while the index stores
--     trigrams case-sensitively, so '^ERROR' carries a key the index does not
--     contain even though ~* genuinely matches every row.  Extraction marks
--     these always_true (the recheck decides); the prefilter ignored that and
--     returned ZERO rows, silently, with no error.  Reported against 3.2.2;
--     the defect dates to the v10 SuRF tier in 3.2.0.  Each must equal its
--     seq-scan ground truth below.
SELECT count(*) AS idx_iregex_upper FROM surf_t WHERE body ~* '^ERROR';
SELECT count(*) AS idx_iregex_lower FROM surf_t WHERE body ~* '^error';
SELECT count(*) AS idx_ilike_upper  FROM surf_t WHERE body ILIKE 'ERROR%';
-- ...and a case-insensitive prefix that genuinely matches nothing stays 0.
SELECT count(*) AS idx_iregex_absent FROM surf_t WHERE body ~* '^ZZQQXJ';

RESET enable_seqscan;

-- Ground-truth via sequential scan.
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS seq_exist FROM surf_t WHERE body %~~ tre_pattern('^error', 0);
SELECT count(*) AS seq_absent FROM surf_t WHERE body %~~ tre_pattern('^zzqqxj', 0);
SELECT count(*) AS seq_unanchored FROM surf_t WHERE body %~~ tre_pattern('refused', 0);
SELECT count(*) AS seq_iregex_upper FROM surf_t WHERE body ~* '^ERROR';
SELECT count(*) AS seq_iregex_lower FROM surf_t WHERE body ~* '^error';
SELECT count(*) AS seq_ilike_upper  FROM surf_t WHERE body ILIKE 'ERROR%';
SELECT count(*) AS seq_iregex_absent FROM surf_t WHERE body ~* '^ZZQQXJ';
RESET enable_indexscan;
RESET enable_bitmapscan;

DROP TABLE surf_t CASCADE;
