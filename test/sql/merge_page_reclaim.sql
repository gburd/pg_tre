-- test/sql/merge_page_reclaim.sql
--
-- Upper-tree page reclaim across a merge.
--
-- A pending-list merge rebuilds the whole upper tree (snapshot_existing_upper
-- + pg_tre_upper_bulkload) and swaps meta.root_upper to the new one.  Every
-- page of the OLD tree -- its upper leaves, its coalesced pages and its
-- out-of-line posting chains -- becomes unreachable at that moment.  Nothing
-- used to free them, so each merge abandoned a full copy of the posting tier:
-- churning 1% of a 60k-row table grew the index 2x, 3x, 4x... with no bound,
-- while page occupancy stayed flat at ~67% (i.e. the growth was orphaned
-- copies, not data).  This was the long-standing stress-scenario-G finding.
--
-- The old tree's pages now go to the deferred free log after the root swap
-- commits, so they return to the FSM and get reused.
--
-- This test asserts the property that actually matters -- growth is BOUNDED
-- across repeated churn -- rather than a specific page count, which varies
-- with build details.  Before the fix round 3 was ~4x baseline and climbing;
-- after it, growth stops once the first orphaned copy is reclaimed.
--
-- It also checks correctness at every round: freeing a page that is still
-- reachable would corrupt results, which is far worse than the leak it
-- replaces.

CREATE EXTENSION IF NOT EXISTS pg_tre;
CREATE EXTENSION IF NOT EXISTS pg_freespacemap;

DROP TABLE IF EXISTS mpr CASCADE;
CREATE TABLE mpr (id serial PRIMARY KEY, body text);

INSERT INTO mpr (body)
SELECT 'government electrification ' || md5(g::text)
FROM generate_series(1, 20000) g;

SET client_min_messages = warning;
CREATE INDEX mpr_tre ON mpr USING tre (body);
RESET client_min_messages;

-- Baseline size, and the ground truth every round must preserve.
CREATE TEMP TABLE mpr_base AS
SELECT pg_relation_size('mpr_tre') AS sz;

SELECT count(*) AS rows_before FROM mpr WHERE body ~ 'government';

-- Three churn rounds: delete 1%, reinsert the same count (row count flat),
-- then VACUUM twice with an XID bump so the free log's XID gate opens.
DELETE FROM mpr WHERE id IN (SELECT id FROM mpr ORDER BY id LIMIT 400);
INSERT INTO mpr (body)
SELECT 'government electrification ' || md5((g + 100000 * 1)::text)
FROM generate_series(1, 400) g;
VACUUM mpr;
SELECT txid_current() IS NOT NULL AS xid_bump_1;
VACUUM mpr;
DELETE FROM mpr WHERE id IN (SELECT id FROM mpr ORDER BY id LIMIT 400);
INSERT INTO mpr (body)
SELECT 'government electrification ' || md5((g + 100000 * 2)::text)
FROM generate_series(1, 400) g;
VACUUM mpr;
SELECT txid_current() IS NOT NULL AS xid_bump_2;
VACUUM mpr;
DELETE FROM mpr WHERE id IN (SELECT id FROM mpr ORDER BY id LIMIT 400);
INSERT INTO mpr (body)
SELECT 'government electrification ' || md5((g + 100000 * 3)::text)
FROM generate_series(1, 400) g;
VACUUM mpr;
SELECT txid_current() IS NOT NULL AS xid_bump_3;
VACUUM mpr;

-- (1) Growth is bounded.  The first merge still orphans one copy of the tree
--     (reclaimed and reused thereafter), so allow up to 3x; the pre-fix
--     behaviour blew past this and kept climbing every round.
--     Fixed builds settle at ~2.0x (one orphaned copy from the first merge,
--     reclaimed and reused thereafter).  With the reclaim disabled the same
--     workload reaches ~3.3x and keeps climbing, so 2.5x cleanly separates
--     the two -- verified against a control build with the reclaim compiled
--     out, which fails this assertion.
SELECT pg_relation_size('mpr_tre') <= 2.5 * (SELECT sz FROM mpr_base)
           AS growth_bounded
FROM mpr_base;

-- (2) Growth has STOPPED, which is the real invariant and the one that fails
--     without the fix.  Three more churn rounds must add essentially nothing:
--     pre-fix each round added a whole copy of the tree (~+100% of baseline
--     every time), so a 10%-of-baseline budget is far below the broken
--     behaviour and far above normal jitter.
--
--     Deliberately not asserted here: a specific free-log page count or FSM
--     free-page count.  The free log is transient staging that may already
--     have drained, and at this table size a merge may orphan too little to
--     leave measurable FSM space -- both make for a test that passes or fails
--     on incidentals rather than on the leak.
CREATE TEMP TABLE mpr_mid AS SELECT pg_relation_size('mpr_tre') AS sz;

DELETE FROM mpr WHERE id IN (SELECT id FROM mpr ORDER BY id LIMIT 400);
INSERT INTO mpr (body)
SELECT 'government electrification ' || md5((g + 100000 * 4)::text)
FROM generate_series(1, 400) g;
VACUUM mpr;
SELECT txid_current() IS NOT NULL AS xid_bump_4;
VACUUM mpr;
DELETE FROM mpr WHERE id IN (SELECT id FROM mpr ORDER BY id LIMIT 400);
INSERT INTO mpr (body)
SELECT 'government electrification ' || md5((g + 100000 * 5)::text)
FROM generate_series(1, 400) g;
VACUUM mpr;
SELECT txid_current() IS NOT NULL AS xid_bump_5;
VACUUM mpr;
DELETE FROM mpr WHERE id IN (SELECT id FROM mpr ORDER BY id LIMIT 400);
INSERT INTO mpr (body)
SELECT 'government electrification ' || md5((g + 100000 * 6)::text)
FROM generate_series(1, 400) g;
VACUUM mpr;
SELECT txid_current() IS NOT NULL AS xid_bump_6;
VACUUM mpr;

SELECT pg_relation_size('mpr_tre') - (SELECT sz FROM mpr_mid)
           <= (SELECT sz FROM mpr_base) / 20
           AS growth_stopped;

-- (3) Correctness across the churn: index result == sequential-scan result.
SET enable_seqscan = off;
SELECT count(*) AS idx_after FROM mpr WHERE body ~ 'government';
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_seqscan = on;
SELECT count(*) AS seq_after FROM mpr WHERE body ~ 'government';
RESET enable_indexscan;
RESET enable_bitmapscan;

DROP TABLE mpr CASCADE;
