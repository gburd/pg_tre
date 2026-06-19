-- Index reloptions (storage parameters).
--
-- Regression guard for the defect where _PG_init did not call
-- pg_tre_init_reloptions(), leaving pg_tre_relopt_kind == 0 so EVERY
-- index storage parameter was silently ignored (with a WARNING) even
-- under shared_preload_libraries.  This test creates an index WITH
-- reloptions and asserts the options round-trip into pg_class.reloptions
-- (NULL/absent before the fix).  Spelling-agnostic (key presence, not
-- the on/off vs true/false rendering).  Single-token robust style.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_tre;
DROP TABLE IF EXISTS reloptions_t;
CREATE TABLE reloptions_t (id serial PRIMARY KEY, body text);
INSERT INTO reloptions_t(body)
SELECT 'reloption probe row ' || g FROM generate_series(1, 200) g;

-- Before the fix this emitted "WARNING: pg_tre: reloptions requested but
-- not initialized" and dropped the options, so reloptions stayed NULL.
CREATE INDEX reloptions_idx ON reloptions_t USING tre (body)
    WITH (fastupdate = off, pending_list_limit = 2048);
RESET client_min_messages;

-- The options must be recorded on the index relation (not NULL) and
-- include both keys we set.
SELECT CASE WHEN
  array_to_string(
    (SELECT reloptions FROM pg_class WHERE relname = 'reloptions_idx'),
    ',') LIKE '%fastupdate=%'
  AND array_to_string(
    (SELECT reloptions FROM pg_class WHERE relname = 'reloptions_idx'),
    ',') LIKE '%pending_list_limit=2048%'
  THEN 'reloptions_applied' ELSE 'reloptions_ignored' END AS r1;

DROP TABLE reloptions_t;
