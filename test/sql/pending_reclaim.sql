-- test/sql/pending_reclaim.sql
--
-- Pending-list page reclaim.  A merge (VACUUM -> amvacuumcleanup ->
-- pg_tre_pending_merge) consumes the pending list into the posting/upper
-- trees and then advances (or clears) meta.pending_head.  Before this
-- fix the consumed pages were simply abandoned -- the code said
-- "orphaned until REINDEX" -- so an index under steady insert traffic
-- leaked its entire pending list on every merge.
--
-- A field report showed 716 pending pages surviving VACUUM (5176 kB vs
-- 1600 kB for the same data freshly built), with the meta page already
-- reporting pending_head = InvalidBlockNumber and pending_n_entries = 0:
-- the entries had merged correctly, the pages were just never freed.
--
-- The consumed pages now go to the deferred free log (the same XID-gated
-- mechanism posting-leaf recycling uses), so they return to the FSM and
-- get reused.
--
-- This test verifies:
--   1. inserting into an EXISTING index populates the pending list
--      (fastupdate is on by default, so aminsert appends there);
--   2. VACUUM merges it and leaves ZERO pending pages behind
--      (pre-fix this stayed at its pre-VACUUM count forever);
--   3. the index still answers correctly after the merge (the pages
--      being freed must be genuinely unreachable -- freeing a live page
--      would corrupt results, which is far worse than the leak);
--   4. repeated insert/VACUUM cycles do not accumulate pending pages.

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS prc CASCADE;
CREATE TABLE prc (id serial PRIMARY KEY, body text);

-- Build the index FIRST, then insert: that routes every entry through
-- the pending list rather than the bulk build path.
SET client_min_messages = warning;
CREATE INDEX prc_tre ON prc USING tre (body);
RESET client_min_messages;

INSERT INTO prc (body) VALUES ('needle'), ('needle'), ('needle');
INSERT INTO prc (body)
SELECT 'filler' || md5(g::text) FROM generate_series(1, 3000) g;

-- (1) The pending list is populated.
SELECT count(*) > 0 AS pending_populated
FROM tre_page_kind_histogram('prc_tre')
WHERE page_kind = 'pending';

-- Ground truth before any merge.
SELECT count(*) AS rows_needle_before FROM prc WHERE body ~ 'needle';

-- (2) VACUUM merges the list and reclaims its pages.  Two passes plus an
--     XID bump: the free log is XID-gated (a page is only handed to the
--     FSM once its deletion XID is globally removable), which is the same
--     discipline nbtree uses for deleted pages.
VACUUM prc;
SELECT txid_current() IS NOT NULL AS bumped_xid;
VACUUM prc;

SELECT coalesce(sum(n_pages), 0) AS pending_pages_after_merge
FROM tre_page_kind_histogram('prc_tre')
WHERE page_kind = 'pending';

-- (3) Results must be unchanged: index vs sequential ground truth.  If
--     the reclaim ever frees a page still reachable from the tree, this
--     is where it shows up.
SET enable_seqscan = off;
SELECT count(*) AS idx_needle FROM prc WHERE body ~ 'needle';
SELECT count(*) AS idx_filler_one FROM prc WHERE body ~ ('filler' || md5('7'));
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_seqscan = on;
SELECT count(*) AS seq_needle FROM prc WHERE body ~ 'needle';
SELECT count(*) AS seq_filler_one FROM prc WHERE body ~ ('filler' || md5('7'));
RESET enable_indexscan;
RESET enable_bitmapscan;

-- (4) Cycle again: pending pages must not accumulate across merges.
INSERT INTO prc (body)
SELECT 'second' || md5(g::text) FROM generate_series(1, 3000) g;
VACUUM prc;
SELECT txid_current() IS NOT NULL AS bumped_xid_2;
VACUUM prc;

SELECT coalesce(sum(n_pages), 0) AS pending_pages_after_second_merge
FROM tre_page_kind_histogram('prc_tre')
WHERE page_kind = 'pending';

-- ...and the second batch is findable through the index.
SET enable_seqscan = off;
SELECT count(*) AS idx_second FROM prc WHERE body ~ ('second' || md5('42'));
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_seqscan = on;
SELECT count(*) AS seq_second FROM prc WHERE body ~ ('second' || md5('42'));
RESET enable_indexscan;
RESET enable_bitmapscan;

DROP TABLE prc CASCADE;
