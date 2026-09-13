-- test/sql/churned_heap_scan.sql
--
-- amgettuple must return HOT-chain ROOT TIDs, on a churned heap.
--
-- The `always_true` scan path (which is what ~* / ILIKE take, since a
-- case-insensitive predicate cannot be trigram-accelerated) streams every
-- live heap TID and lets the executor recheck.  It collected those TIDs from
-- heap_getnext(), which returns the CURRENT tuple version -- and after any
-- HOT update that version is a HEAP_ONLY successor, not the chain root.
--
-- index_fetch_heap -> heap_hot_search_buffer walks forward from the TID it is
-- given and bails immediately if that TID is itself heap-only
-- (`at_chain_start && HeapTupleIsHeapOnly`).  So every HOT-updated row was
-- handed to the executor as an unreachable TID and silently dropped:
--
--   fresh index                    -> exact
--   after 1 no-VACUUM UPDATE pass  -> lost 1 of 3
--   after 3 passes                 -> lost 16 of 1473
--   reporter's production heap      -> lost 169 of 185 (27.2M lifetime updates)
--
-- amgetbitmap was immune throughout, because it reports whole blocks and lets
-- the executor recheck, so the index's tuple-level TIDs never matter.  That
-- asymmetry is why three rounds of casing-parameterised tests missed this:
-- they all planned as bitmap scans.
--
-- This test therefore pins BOTH axes the reporter kept asking for:
--   1. scan path -- every case is run under forced amgettuple AND forced
--      amgetbitmap, each required to equal the sequential-scan ground truth;
--   2. heap state -- the same index is checked fresh AND after no-VACUUM
--      UPDATE churn, because a suite that only indexes freshly-inserted rows
--      cannot see this class of bug at all.

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS churn CASCADE;
CREATE TABLE churn (id bigserial PRIMARY KEY, name text);

INSERT INTO churn (name) VALUES ('git'), ('git'), ('git');
INSERT INTO churn (name)
SELECT 'pkg' || md5(g::text) FROM generate_series(1, 2000) g;

SET client_min_messages = warning;
CREATE INDEX churn_tre ON churn USING tre (name);
RESET client_min_messages;
ANALYZE churn;

-- ---- fresh heap: all three paths must agree ----
SET enable_seqscan = off; SET enable_bitmapscan = off;   -- amgettuple
SELECT count(*) AS fresh_gettuple_git FROM churn WHERE name ~* '^git';
SELECT count(*) AS fresh_gettuple_g   FROM churn WHERE name ~* '^g';
SET enable_seqscan = off; SET enable_indexscan = off; SET enable_bitmapscan = on;
SELECT count(*) AS fresh_bitmap_git   FROM churn WHERE name ~* '^git';
SELECT count(*) AS fresh_bitmap_g     FROM churn WHERE name ~* '^g';
RESET enable_indexscan; RESET enable_bitmapscan; SET enable_seqscan = on;
SET enable_indexscan = off; SET enable_bitmapscan = off;  -- ground truth
SELECT count(*) AS fresh_seq_git      FROM churn WHERE name ~* '^git';
SELECT count(*) AS fresh_seq_g        FROM churn WHERE name ~* '^g';
RESET enable_indexscan; RESET enable_bitmapscan;

-- ---- churn the heap: three UPDATE passes, deliberately no VACUUM ----
-- `SET name = name` is a HOT update (no indexed column changes value), which
-- is exactly the shape that produces heap-only successors.
UPDATE churn SET name = name;
UPDATE churn SET name = name;
UPDATE churn SET name = name;
ANALYZE churn;

-- The heap is now mostly heap-only tuples with the visibility map unset.
SELECT (SELECT relallvisible FROM pg_class WHERE relname = 'churn') = 0
           AS visibility_map_unset;

-- ---- churned heap: all three paths must STILL agree ----
SET enable_seqscan = off; SET enable_bitmapscan = off;   -- amgettuple
SELECT count(*) AS churned_gettuple_git FROM churn WHERE name ~* '^git';
SELECT count(*) AS churned_gettuple_g   FROM churn WHERE name ~* '^g';
SELECT count(*) AS churned_gettuple_un  FROM churn WHERE name ~* 'git';
SET enable_seqscan = off; SET enable_indexscan = off; SET enable_bitmapscan = on;
SELECT count(*) AS churned_bitmap_git   FROM churn WHERE name ~* '^git';
SELECT count(*) AS churned_bitmap_g     FROM churn WHERE name ~* '^g';
SELECT count(*) AS churned_bitmap_un    FROM churn WHERE name ~* 'git';
RESET enable_indexscan; RESET enable_bitmapscan; SET enable_seqscan = on;
SET enable_indexscan = off; SET enable_bitmapscan = off;  -- ground truth
SELECT count(*) AS churned_seq_git      FROM churn WHERE name ~* '^git';
SELECT count(*) AS churned_seq_g        FROM churn WHERE name ~* '^g';
SELECT count(*) AS churned_seq_un       FROM churn WHERE name ~* 'git';
RESET enable_indexscan; RESET enable_bitmapscan;

-- Case-sensitive operators take the indexed path rather than always_true,
-- so check them over the churned heap too.
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) AS churned_gettuple_sens FROM churn WHERE name %~~ tre_pattern('git', 0);
SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_seqscan = on;
SELECT count(*) AS churned_seq_sens      FROM churn WHERE name %~~ tre_pattern('git', 0);
RESET enable_indexscan; RESET enable_bitmapscan;

DROP TABLE churn CASCADE;
