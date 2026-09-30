-- test/sql/vacuum_pending_dead.sql
--
-- A dead TID still in the pending list must not outlive its heap tuple.
--
-- ambulkdelete stripped dead TIDs only from the posting trees.  A row
-- inserted and deleted between two VACUUMs is still in the pending list,
-- so it survived the strip, was merged into a tree by amvacuumcleanup
-- right after, and stayed there -- while the heap freed the line pointer
-- and truncated the block.  The next index scan fetched a TID past the
-- heap's end: "could not read blocks 1..1 ... read only 0 of 8192 bytes"
-- (field report 2026-09-29).  ambulkdelete now merges the pending list
-- first, as ginbulkdelete does.

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS vpd;
CREATE TABLE vpd (id serial PRIMARY KEY, body text)
    WITH (autovacuum_enabled = off);
SET client_min_messages = warning;
CREATE INDEX vpd_tre ON vpd USING tre (body);
RESET client_min_messages;

-- Fills several heap blocks; every entry goes to the pending list.
INSERT INTO vpd (body)
SELECT 'ghost_' || g || repeat(' pad', 40) FROM generate_series(1, 5000) g;
SELECT pg_relation_size('vpd') / 8192 > 1 AS several_heap_blocks;

DELETE FROM vpd;
VACUUM vpd;
SELECT pg_relation_size('vpd') AS heap_bytes_after_truncate;

INSERT INTO vpd (body) VALUES ('alive');

SET enable_seqscan = off;
SELECT count(*) AS ghosts_via_index FROM vpd WHERE body ~ 'ghost_4';
SELECT count(*) AS alive_via_index FROM vpd WHERE body ~ 'alive';
RESET enable_seqscan;

-- Same through flush_to_run, whose merges land in catalog runs rather
-- than the base tree; ambulkdelete must walk those too.
TRUNCATE vpd;
SET pg_tre.flush_to_run = on;
INSERT INTO vpd (body)
SELECT 'wraith_' || g || repeat(' pad', 40) FROM generate_series(1, 5000) g;
VACUUM vpd;                 -- flushes the pending list into a run
DELETE FROM vpd;
VACUUM vpd;
RESET pg_tre.flush_to_run;
INSERT INTO vpd (body) VALUES ('alive');
SET enable_seqscan = off;
SELECT count(*) AS wraiths_via_index FROM vpd WHERE body ~ 'wraith_4';
RESET enable_seqscan;

DROP TABLE vpd;
