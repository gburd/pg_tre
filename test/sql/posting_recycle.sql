-- test/sql/posting_recycle.sql
--
-- FSM page-freeing (1.5.5).  Before 1.5.5 an out-of-line posting leaf
-- that VACUUM emptied was repacked to a zero-entry leaf but never
-- physically reclaimed: the block stayed allocated forever (only a
-- full REINDEX shrank the index).  1.5.5 adds nbtree-style deferred
-- page deletion + recycle:
--
--   * When VACUUM empties a NON-head posting leaf it splices the leaf
--     out of the right-link chain (predecessor's right_link is advanced
--     past it) and marks it PG_TRE_LEAF_DELETED with a deletion XID,
--     leaving a coherent empty waypoint so a concurrent scan holding a
--     stale right_link still terminates correctly.
--   * A later VACUUM's cleanup pass physically recycles every DELETED
--     leaf whose deletion XID has aged past the global visibility
--     horizon, recording the block into the index FSM
--     (RecordFreeIndexPage) for reuse by future page allocations.
--
-- This test builds a multi-leaf posting chain, deletes an interior TID
-- range to empty interior leaves, VACUUMs to unlink and then recycle
-- them, and verifies (a) index scans still agree with the heap across
-- the unlink/recycle, (b) pages actually land in the FSM, and (c) the
-- freed pages are reused (the index does not grow, and the FSM free
-- count drops) when new rows are inserted afterwards.

CREATE EXTENSION IF NOT EXISTS pg_tre;
CREATE EXTENSION IF NOT EXISTS pg_freespacemap;

DROP TABLE IF EXISTS prec CASCADE;
CREATE TABLE prec (id serial PRIMARY KEY, body text);

-- 20K rows all sharing the trigram 'the' force that posting list to
-- split across many right-linked leaves.  The id is embedded so the
-- packed TID order follows insertion order, letting us empty a
-- contiguous middle band of leaves by deleting a contiguous id range.
INSERT INTO prec(body)
SELECT 'the row number ' || g
FROM generate_series(1, 20000) g;

SET client_min_messages = 'warning';
CREATE INDEX prec_idx ON prec USING tre (body);
RESET client_min_messages;

-- Drain the pending list into the posting tree.
VACUUM prec;

CREATE OR REPLACE FUNCTION prec_diff(pat text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE seq_ids int[]; idx_ids int[];
BEGIN
  SET LOCAL enable_indexscan=off;
  SET LOCAL enable_bitmapscan=off;
  SET LOCAL enable_seqscan=on;
  EXECUTE format('SELECT array_agg(id ORDER BY id) FROM prec '
                 'WHERE tre_amatch(body, %L, 0)', pat) INTO seq_ids;
  SET LOCAL enable_seqscan=off;
  SET LOCAL enable_indexscan=on;
  SET LOCAL enable_bitmapscan=on;
  EXECUTE format('SELECT array_agg(id ORDER BY id) FROM prec '
                 'WHERE body %%~~ tre_pattern(%L, 0)', pat) INTO idx_ids;
  IF seq_ids IS NOT DISTINCT FROM idx_ids THEN
    RETURN 'OK  ' || pat;
  END IF;
  RETURN format('BAD %s  seq=%s  idx=%s', pat, cardinality(seq_ids),
                cardinality(idx_ids));
END$$;

-- Helper: how many main-fork blocks does the FSM currently advertise as
-- (near-)entirely free?  Recycled posting leaves are recorded fully
-- free, so this rises after the recycle pass runs and falls again as
-- later allocations draw the blocks back out.
CREATE OR REPLACE FUNCTION prec_free_pages() RETURNS bigint
LANGUAGE sql AS $$
  SELECT count(*) FROM pg_freespace('prec_idx'::regclass)
  WHERE avail > 8000;
$$;

-- Baseline agreement between index and heap.
SELECT prec_diff('the');
SELECT prec_diff('row');

-- Capture the index size before deletion so we can prove the freed
-- pages get reused (rather than the relation extending) later.
SELECT pg_relation_size('prec_idx') AS size_before \gset

-- Delete a large contiguous interior band.  These ids map to a
-- contiguous run of packed TIDs, so whole interior posting leaves drain
-- to empty and become eligible for unlink.
DELETE FROM prec WHERE id BETWEEN 4000 AND 16000;

-- First VACUUM: ambulkdelete repacks survivors and unlinks the emptied
-- interior leaves (marks them PG_TRE_LEAF_DELETED).  amvacuumcleanup's
-- recycle pass also runs; whether a given deleted leaf is recyclable yet
-- depends on the visibility horizon.
VACUUM prec;

-- Index still agrees with the heap immediately after the unlink: a
-- deleted waypoint matches nothing and stale right_links still resolve.
SELECT prec_diff('the');
SELECT prec_diff('row');

-- Recycle is deferred behind the global visibility horizon: a deleted
-- waypoint can only be physically reclaimed once no snapshot could still
-- be mid-traversal through a stale right_link (nbtree's safexid gate,
-- GlobalVisCheckRemovableFullXid).  Immediately after the unlink the
-- deleting transaction is typically still ahead of that horizon, so the
-- first cleanup pass leaves the deleted leaves pending.  Advance the XID
-- horizon (the assignments consume XIDs without emitting nondeterministic
-- output) so the deleting xact ages out and the leaves become recyclable.
DO $$ BEGIN PERFORM txid_current(); PERFORM txid_current(); PERFORM txid_current(); END $$;

-- Second VACUUM: the deleting transaction is now behind the global
-- visibility horizon, so the cleanup pass recycles the deleted leaves
-- into the FSM.
VACUUM prec;

-- The FSM now advertises reclaimed posting-leaf blocks as free.
SELECT prec_free_pages() > 0 AS pages_were_freed;

-- Index/heap still agree after recycle.
SELECT prec_diff('the');
SELECT prec_diff('row');

-- Reuse check: insert fresh rows.  New page allocations (here, pending
-- pages extended on the insert path) draw from the recycled blocks in
-- the FSM, so the index must not grow beyond its pre-delete size and the
-- FSM free count must fall as the blocks are handed back out.
SELECT prec_free_pages() AS free_before_reuse \gset
INSERT INTO prec(body)
SELECT 'the row number ' || g
FROM generate_series(20001, 30000) g;

SELECT prec_free_pages() < :free_before_reuse AS freed_pages_reused;
SELECT pg_relation_size('prec_idx') <= :size_before AS index_did_not_grow;

-- Final correctness: index agrees with the heap over the full mutated
-- data set (original survivors + the reinserted band).
SELECT prec_diff('the');
SELECT prec_diff('row');
SELECT prec_diff('number');

DROP FUNCTION prec_diff(text);
DROP FUNCTION prec_free_pages();
DROP INDEX prec_idx;
DROP TABLE prec;
