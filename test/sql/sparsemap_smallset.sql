-- test/sql/sparsemap_smallset.sql
--
-- sparsemap 5.7.0 adds "small-set mode": a map confined to the low 1024 bits
-- may be stored as a bare uint64 word array (PostgreSQL Bitmapset layout)
-- instead of the chunk form, selected by the top bit of the 8-byte header.
--
-- "May be", not "is": the cap is a precondition, not the rule.  5.7.0 picks
-- whichever of three encodings is SMALLEST -- flat small form, sparse chunk, or
-- a single descriptor-only RLE chunk -- so a low-but-isolated index stays in
-- chunk mode (measured: {0} and {0,1,5,63} are SMALL at 16 B, but {900} alone
-- is 32 B as a chunk versus 128 B flat, so it stays chunk).  Which encoding a
-- given posting lands in is therefore not something this test can assert; what
-- it asserts is that reads are correct whichever one is chosen.
--
-- This matters to pg_tre specifically because of how it packs a TID:
--
--     pg_tre_pack_tid() = (blk << 16) | off     [include/pg_tre/page.h]
--
-- so an index below 1024 means heap block 0 with a low offset.  That is not an
-- exotic corner: it is every small table, and every table in this very test
-- suite.  So pg_tre writes small-mode maps routinely, and the encoding has to
-- be exercised on the real read paths rather than assumed equivalent.
--
-- Two properties are pinned here:
--
--  1. Correct reads for maps entirely below the cap (pure small mode), maps
--     entirely above it (pure chunk mode), and maps that STRADDLE the cap --
--     the promote/demote boundary, where an encoding switch happens mid-life.
--
--  2. The 4.1.0 corruption guard does not false-positive on the new encoding.
--     That guard fires when sm_get_size(map) != the byte length pg_tre handed
--     in; a new encoding whose reported size disagreed would turn every small
--     index into a spurious ERRCODE_DATA_CORRUPTED.  Measured at the library
--     level as 0 false positives across 10 shapes (4 genuinely small-mode);
--     this is the in-server half of that check -- if it ever regresses, these
--     queries raise "corrupt inline sparsemap" instead of returning rows.
--
-- Every count is asserted against seq-scan ground truth through BOTH scan
-- paths, because an encoding bug that under-returns is exactly the class of
-- failure this index has shipped before.

-- Quiet the "already exists" notice so the expected output does not depend on
-- whether an earlier test in the run already created the extension.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_tre;
RESET client_min_messages;

-- ---------------------------------------------------------------------------
-- Case 1: pure small mode.  Few enough rows that every TID is block 0 with a
-- low offset, so every posting map is below the 1024-bit cap.
-- ---------------------------------------------------------------------------
DROP TABLE IF EXISTS sm_small CASCADE;
CREATE TABLE sm_small (id serial PRIMARY KEY, body text);
INSERT INTO sm_small (body) VALUES
    ('alpha'), ('alpha'), ('beta'), ('gamma alpha'), ('delta');

SET client_min_messages = warning;
CREATE INDEX sm_small_tre ON sm_small USING tre (body);
RESET client_min_messages;

SET enable_seqscan = off;
SELECT count(*) AS idx_alpha FROM sm_small WHERE body ~ 'alpha';
SELECT count(*) AS idx_beta  FROM sm_small WHERE body ~ 'beta';
SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_seqscan = on;
SELECT count(*) AS seq_alpha FROM sm_small WHERE body ~ 'alpha';
SELECT count(*) AS seq_beta  FROM sm_small WHERE body ~ 'beta';
RESET enable_indexscan; RESET enable_bitmapscan;

-- Force the bitmap path too: a small-mode map must serve amgetbitmap
-- identically to amgettuple.
SET enable_seqscan = off; SET enable_indexscan = off;
SELECT count(*) AS bitmap_alpha FROM sm_small WHERE body ~ 'alpha';
RESET enable_indexscan; RESET enable_seqscan;

-- ---------------------------------------------------------------------------
-- Case 2: straddle the cap.  Enough rows to push TIDs past index 1024 so the
-- map promotes from small to chunk mode partway through the build, then delete
-- the high rows and VACUUM so it can demote again.  The repack path reads and
-- rewrites these maps, so both directions get exercised.
-- ---------------------------------------------------------------------------
DROP TABLE IF EXISTS sm_straddle CASCADE;
CREATE TABLE sm_straddle (id serial PRIMARY KEY, body text);
INSERT INTO sm_straddle (body)
SELECT 'hot' FROM generate_series(1, 4000) g;          -- spans many blocks
INSERT INTO sm_straddle (body)
SELECT 'cold' || md5(g::text) FROM generate_series(1, 500) g;

SET client_min_messages = warning;
CREATE INDEX sm_straddle_tre ON sm_straddle USING tre (body);
RESET client_min_messages;
ANALYZE sm_straddle;

SET enable_seqscan = off;
SELECT count(*) AS idx_hot FROM sm_straddle WHERE body ~ 'hot';
SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_seqscan = on;
SELECT count(*) AS seq_hot FROM sm_straddle WHERE body ~ 'hot';
RESET enable_indexscan; RESET enable_bitmapscan;

-- Delete most of the high TIDs, then vacuum: the surviving map shrinks back
-- toward (and possibly below) the small cap, taking the demote path.
DELETE FROM sm_straddle WHERE body = 'hot' AND id > 100;
VACUUM sm_straddle;
SELECT txid_current() IS NOT NULL AS xid_bump;
VACUUM sm_straddle;

SET enable_seqscan = off;
SELECT count(*) AS idx_hot_after FROM sm_straddle WHERE body ~ 'hot';
SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_seqscan = on;
SELECT count(*) AS seq_hot_after FROM sm_straddle WHERE body ~ 'hot';
RESET enable_indexscan; RESET enable_bitmapscan;

-- ---------------------------------------------------------------------------
-- Case 3: a contiguous low run.  5.7.0's promote decision is RLE-aware -- a
-- dense run from bit 0 encodes as a single ~24-byte RLE chunk rather than the
-- flat small form, so this shape takes a third code path distinct from both
-- cases above.
-- ---------------------------------------------------------------------------
DROP TABLE IF EXISTS sm_run CASCADE;
CREATE TABLE sm_run (id serial PRIMARY KEY, body text);
INSERT INTO sm_run (body) SELECT 'runrunrun' FROM generate_series(1, 200) g;

SET client_min_messages = warning;
CREATE INDEX sm_run_tre ON sm_run USING tre (body);
RESET client_min_messages;

SET enable_seqscan = off;
SELECT count(*) AS idx_run FROM sm_run WHERE body ~ 'runrun';
SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_seqscan = on;
SELECT count(*) AS seq_run FROM sm_run WHERE body ~ 'runrun';
RESET enable_indexscan; RESET enable_bitmapscan;

-- Round-trip the on-disk form: amcheck-style rebuild must agree.
REINDEX INDEX sm_run_tre;
SET enable_seqscan = off;
SELECT count(*) AS idx_run_reindexed FROM sm_run WHERE body ~ 'runrun';
RESET enable_seqscan;

DROP TABLE sm_small CASCADE;
DROP TABLE sm_straddle CASCADE;
DROP TABLE sm_run CASCADE;
