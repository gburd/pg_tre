-- Posting-page coalescing: VACUUM / merge / delete safety (v8).
--
-- Coalescing packs medium-cardinality postings onto shared pages whose
-- upper-tree entries carry a (COALESCED_FLAG | slot) marker in
-- inline_bytes and the coalesced-page block in posting_root.  Several
-- maintenance paths historically read inline_bytes as a blob length or
-- posting_root as a posting-tree root, which would corrupt on a
-- coalesced entry.  This test exercises those paths with coalescing on
-- and asserts correctness (results == seq-scan) survives DELETE+VACUUM
-- (ambulkdelete over coalesced entries) and INSERT+VACUUM (pending
-- merge materializing existing coalesced postings).  Single-token
-- robust style; NOTICEs suppressed before any DDL so the output does
-- not depend on prior catalog state.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_tre;
DROP TABLE IF EXISTS cv_t;
CREATE TABLE cv_t (id serial PRIMARY KEY, body text);

INSERT INTO cv_t(body)
SELECT 'noise' || g::text ||
       (CASE WHEN (g * 2654435761) % 3 = 0 THEN ' midtoken' ELSE '' END) ||
       (CASE WHEN g % 97 = 0 THEN ' raretoken' ELSE '' END)
FROM generate_series(1, 3000) g;

SET pg_tre.coalesce_enable = on;
CREATE INDEX cv_idx ON cv_t USING tre (body);

SELECT CASE WHEN
  (SELECT count(*) FROM cv_t WHERE body %~~ tre_pattern('midtoken', 0)) =
  (SELECT count(*) FROM cv_t WHERE body LIKE '%midtoken%')
  THEN 'built_ok' ELSE 'built_bad' END AS r1;

DELETE FROM cv_t WHERE id % 5 = 0;
VACUUM cv_t;
SELECT CASE WHEN
  (SELECT count(*) FROM cv_t WHERE body %~~ tre_pattern('midtoken', 0)) =
  (SELECT count(*) FROM cv_t WHERE body LIKE '%midtoken%')
  THEN 'postvacuum_ok' ELSE 'postvacuum_bad' END AS r2;

INSERT INTO cv_t(body)
SELECT 'fresh' || g::text || ' midtoken'
FROM generate_series(1, 200) g;
VACUUM cv_t;
SELECT CASE WHEN
  (SELECT count(*) FROM cv_t WHERE body %~~ tre_pattern('midtoken', 0)) =
  (SELECT count(*) FROM cv_t WHERE body LIKE '%midtoken%')
  THEN 'postinsert_ok' ELSE 'postinsert_bad' END AS r3;

SET pg_tre.coalesce_enable = off;
DROP TABLE cv_t;
