-- test/sql/sparsemap_corrupt_guard.sql
--
-- A corrupt on-page sparsemap must ERROR, not read as zero TIDs.
--
-- sparsemap 5.6.0 validates on open.  sm_open() cannot report failure (it
-- returns void), so for bytes it rejects it substitutes an EMPTY map.  Left
-- unchecked that turns a damaged posting leaf into silently-wrong query
-- results -- and, worse, a VACUUM of that leaf would repack it as though it
-- genuinely held no TIDs, making the loss permanent.
--
-- pg_tre therefore cross-checks, after every sm_open(), that the map still
-- reports the byte count it was handed -- sm_get_size(map) != n -- and raises
-- ERRCODE_DATA_CORRUPTED with a REINDEX hint on all three read paths
-- (inline blob, out-of-line leaf, vacuum repack).
--
-- Note sm_validate() is NOT the right predicate and was measured to be
-- useless here: the empty map sm_open substitutes is itself structurally
-- valid, so sm_validate returns true on every corruption (0 of 4 detected).
-- The size cross-check catches 4 of 4 with no false positive on any map shape
-- pg_tre emits.
--
-- This test only asserts the NEGATIVE half -- that a healthy index built and
-- read by this version never trips the guard -- because deliberately
-- corrupting a page from SQL is not portable.  The positive half (a corrupted
-- byte range really is rejected) is covered directly against the vendored
-- library in the C-level qualification; see the release notes.
--
-- What this pins is the property that matters for upgrades: every map shape
-- pg_tre produces must PASS the stricter validator, so the guard is silent in
-- normal operation.  If a future sparsemap tightens validation in a way that
-- rejects pg_tre's own output, this test fails loudly instead of the index
-- quietly erroring in production.

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS smguard CASCADE;
CREATE TABLE smguard (id bigserial PRIMARY KEY, body text);

-- Span the map shapes pg_tre emits: singletons, short runs, long dense runs
-- (RLE), wide sparse spreads, and enough rows to force multi-leaf postings.
INSERT INTO smguard (body) VALUES ('solo');
INSERT INTO smguard (body)
SELECT 'dense' FROM generate_series(1, 3000) g;          -- one hot trigram
INSERT INTO smguard (body)
SELECT 'sparse' || md5(g::text) FROM generate_series(1, 3000) g;
INSERT INTO smguard (body)
SELECT 'wide' || (g * 977) FROM generate_series(1, 1500) g;

SET client_min_messages = warning;
CREATE INDEX smguard_tre ON smguard USING tre (body);
RESET client_min_messages;
ANALYZE smguard;

-- Reads must succeed (no corruption error) and agree with ground truth.
SET enable_seqscan = off;
SELECT count(*) AS idx_dense  FROM smguard WHERE body ~ 'dense';
SELECT count(*) AS idx_solo   FROM smguard WHERE body ~ 'solo';
SELECT count(*) AS idx_sparse FROM smguard WHERE body ~ 'sparse';
SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_seqscan = on;
SELECT count(*) AS seq_dense  FROM smguard WHERE body ~ 'dense';
SELECT count(*) AS seq_solo   FROM smguard WHERE body ~ 'solo';
SELECT count(*) AS seq_sparse FROM smguard WHERE body ~ 'sparse';
RESET enable_indexscan; RESET enable_bitmapscan;

-- The vacuum path takes the other guarded sm_wrap()+sm_open() site: deleting
-- rows and vacuuming walks posting leaves through it.  This must not error.
DELETE FROM smguard WHERE body = 'dense' AND id % 3 = 0;
VACUUM smguard;
SELECT txid_current() IS NOT NULL AS xid_bump;
VACUUM smguard;

-- ...and results still agree after the repack.
SET enable_seqscan = off;
SELECT count(*) AS idx_after FROM smguard WHERE body ~ 'dense';
SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_seqscan = on;
SELECT count(*) AS seq_after FROM smguard WHERE body ~ 'dense';
RESET enable_indexscan; RESET enable_bitmapscan;

-- Multi-leaf postings exercise the right-link walk, which reopens each leaf.
SET enable_seqscan = off;
SELECT count(*) AS idx_wide FROM smguard WHERE body ~ 'wide';
SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_seqscan = on;
SELECT count(*) AS seq_wide FROM smguard WHERE body ~ 'wide';
RESET enable_indexscan; RESET enable_bitmapscan;

DROP TABLE smguard CASCADE;
