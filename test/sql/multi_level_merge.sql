-- test/sql/multi_level_merge.sql
--
-- Two 1.5.6 hardening behaviours:
--
--   1. Multi-level upper-tree pending merge.  Before 1.5.6 the pending
--      merge could read back only a SINGLE upper leaf; once enough
--      distinct trigrams accumulated that VACUUM had to build a
--      multi-LEVEL upper tree (an UPPER internal page, page_kind=2,
--      fanning out to many UPPER leaves, page_kind=3), the next merge
--      raised "pending-list merge on multi-level upper tree not yet
--      supported" and demanded a REINDEX.  1.5.6 makes
--      snapshot_existing_upper descend the whole upper subtree
--      recursively, so a second batch of inserts merges cleanly.
--
--   2. pg_tre.compile_timeout_ms enforcement.  The GUC was previously
--      defined but never enforced; 1.5.6 arms a real wall-clock
--      deadline around regex compilation (via a progress hook woven
--      into TRE's NFA-build loops) and raises a cancel error when a
--      pathological pattern blows the budget.
--
-- Both behaviours are deterministic: the multi-level case is asserted
-- via a page-kind histogram (an UPPER internal page must exist after
-- the build, and a second batch + VACUUM must not error and must still
-- agree with the heap), and the timeout case via a pattern whose
-- bounded-repetition expansion reliably exceeds a 1 ms budget.

CREATE EXTENSION IF NOT EXISTS pg_tre;
CREATE EXTENSION IF NOT EXISTS pageinspect;

-- ------------------------------------------------------------------
-- Part 1: multi-level upper-tree pending merge
-- ------------------------------------------------------------------

DROP TABLE IF EXISTS mlm CASCADE;
CREATE TABLE mlm (id serial PRIMARY KEY, body text);

-- 4000 distinct md5 hashes generate far more distinct trigrams than fit
-- in a single upper leaf, forcing VACUUM to build a multi-LEVEL upper
-- tree (>= 1 UPPER internal page).
INSERT INTO mlm(body) SELECT md5(g::text) FROM generate_series(1, 4000) g;

SET client_min_messages = 'warning';
CREATE INDEX mlm_idx ON mlm USING tre (body);
RESET client_min_messages;

-- Drain the pending list into the posting/upper tree.
VACUUM mlm;

-- Count UPPER internal pages (page_kind = PG_TRE_PAGE_UPPER = 2).  The
-- page-kind tag is the first byte of each page's special area, whose
-- offset we read from the page header (robust to special-area size).
CREATE OR REPLACE FUNCTION mlm_internal_pages() RETURNS bigint
LANGUAGE sql AS $$
  SELECT count(*) FROM (
    SELECT get_byte(get_raw_page('mlm_idx', b),
                    (page_header(get_raw_page('mlm_idx', b))).special) AS kind
    FROM generate_series(0, pg_relation_size('mlm_idx')/8192 - 1) b
  ) s
  WHERE kind = 2;
$$;

-- A multi-level tree exists: at least one UPPER internal page.
SELECT mlm_internal_pages() > 0 AS has_multi_level_upper_tree;

CREATE OR REPLACE FUNCTION mlm_diff(pat text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE seq_ids int[]; idx_ids int[];
BEGIN
  SET LOCAL enable_indexscan=off;
  SET LOCAL enable_bitmapscan=off;
  SET LOCAL enable_seqscan=on;
  EXECUTE format('SELECT array_agg(id ORDER BY id) FROM mlm '
                 'WHERE tre_amatch(body, %L, 0)', pat) INTO seq_ids;
  SET LOCAL enable_seqscan=off;
  SET LOCAL enable_indexscan=on;
  SET LOCAL enable_bitmapscan=on;
  EXECUTE format('SELECT array_agg(id ORDER BY id) FROM mlm '
                 'WHERE body %%~~ tre_pattern(%L, 0)', pat) INTO idx_ids;
  IF seq_ids IS NOT DISTINCT FROM idx_ids THEN
    RETURN 'OK  ' || pat;
  END IF;
  RETURN format('BAD %s  seq=%s  idx=%s', pat, cardinality(seq_ids),
                cardinality(idx_ids));
END$$;

-- Baseline agreement after the first build.
SELECT mlm_diff('abc');
SELECT mlm_diff('00');

-- Second batch.  Before 1.5.6 this VACUUM raised:
--   ERROR: pg_tre: pending-list merge on multi-level upper tree not yet
--          supported  (HINT: REINDEX ...)
-- Now it must merge cleanly into the existing multi-level tree.
INSERT INTO mlm(body) SELECT md5(g::text) FROM generate_series(4001, 8000) g;
VACUUM mlm;

-- Still multi-level, and the index still agrees with the heap over the
-- merged data set.
SELECT mlm_internal_pages() > 0 AS still_multi_level;
SELECT mlm_diff('abc');
SELECT mlm_diff('00');
SELECT mlm_diff('def');

DROP FUNCTION mlm_internal_pages();
DROP FUNCTION mlm_diff(text);
DROP INDEX mlm_idx;
DROP TABLE mlm;

-- ------------------------------------------------------------------
-- Part 2: pg_tre.compile_timeout_ms enforcement
-- ------------------------------------------------------------------

-- Raise the NFA-state cap out of the way so the timeout (not the state
-- guard) is what trips, and confirm the GUC's visible default.
SHOW pg_tre.compile_timeout_ms;

-- A pattern with deeply nested bounded repetitions whose AST expansion
-- reliably takes far longer than 1 ms to compile.  With the timeout
-- armed the compile is pre-empted and a cancel error is raised.
SET pg_tre.max_nfa_states = 1000000;
SET pg_tre.compile_timeout_ms = 1;
SELECT tre_amatch('x', 'a{80}{80}{80}', 0);

-- A trivial pattern compiles well within a generous budget (proving the
-- deadline only fires for genuinely expensive compiles, and is disarmed
-- cleanly afterwards).
SET pg_tre.compile_timeout_ms = 5000;
SELECT tre_amatch('hello', 'hello', 0);

RESET ALL;
