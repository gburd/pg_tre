-- Phase B1: run/level catalog (format v7).
-- A freshly built index has exactly one implicit run (run_id 0,
-- level 1) rooted at the index roots, and its scan results are
-- identical to a forced seq scan.  This is the "worst case = today"
-- guarantee: one run behaves exactly like the pre-v7 structure.
CREATE EXTENSION IF NOT EXISTS pg_tre;

CREATE TABLE rc_t(id serial primary key, body text);
INSERT INTO rc_t(body)
SELECT md5(g::text) || (CASE WHEN g % 50 = 0 THEN ' findme' ELSE '' END)
FROM generate_series(1, 2000) g;
CREATE INDEX rc_idx ON rc_t USING tre (body);
ANALYZE rc_t;

-- Exactly one live run, the implicit single run.
SELECT count(*) AS n_runs FROM tre_run_catalog_status('rc_idx');
SELECT run_id, level FROM tre_run_catalog_status('rc_idx');

-- The index is at format v9.
SELECT format_version FROM pg_tre_index_format_status('rc_idx');

-- Scan result equals seq-scan ground truth.
SET enable_seqscan = off;
SELECT count(*) AS idx_count FROM rc_t WHERE body %~~ tre_pattern('findme', 0);
SET enable_seqscan = on; SET enable_indexscan = off; SET enable_bitmapscan = off;
SELECT count(*) AS seq_count FROM rc_t WHERE body %~~ tre_pattern('findme', 0);

-- upgrade_index on an already-v7 index is a no-op; results unchanged.
SELECT pg_tre_upgrade_index('rc_idx');
SET enable_seqscan = off;
SELECT count(*) AS post_upgrade FROM rc_t WHERE body %~~ tre_pattern('findme', 0);

DROP TABLE rc_t;
