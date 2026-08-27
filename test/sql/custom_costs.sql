-- test/sql/custom_costs.sql
-- Regression test: the %~~ index recheck honors a pattern's per-edit
-- cost weights (cost_ins, cost_del, cost_subst), matching the seq-scan
-- tre_amatch(text, text, k, ci, cd, cs) UDF.
--
-- Before this fix the operator recheck (tre_match_scalar) called the
-- unit-cost path and silently ignored non-uniform costs, so an indexed
-- %~~ scan could return different rows than the equivalent function
-- call for a pattern built with tre_pattern_make_full().

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS cost_t CASCADE;
CREATE TABLE cost_t (id serial, body text);

-- 'abcd'  : exact
-- 'abxd'  : one substitution from 'abcd'
-- 'abd'   : one deletion from 'abcd'
-- 'abccd' : one insertion into 'abcd'
INSERT INTO cost_t (body) VALUES
  ('abcd zzz'), ('abxd zzz'), ('abd zzz'), ('abccd zzz'), ('wxyz zzz');

CREATE INDEX cost_idx ON cost_t USING tre (body);

-- Build a pattern with expensive substitution (cost_subst = 5) but a
-- max_cost budget of 1.  A substitution therefore costs 5 > 1 and must
-- NOT match; a deletion/insertion (cost 1) still may.  The expensive
-- construction uses tre_pattern(pattern, max_cost, ci, cd, cs).

-- Reference: the seq-scan function form with the same weights.
SET enable_seqscan = off;
SELECT id, body FROM cost_t
WHERE body %~~ tre_pattern('abcd', 1, 1, 1, 5)
ORDER BY id;

-- Same predicate but forced through a seq scan: must be identical.
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT id, body FROM cost_t
WHERE body %~~ tre_pattern('abcd', 1, 1, 1, 5)
ORDER BY id;

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;

-- Now cheap substitution (default 1,1,1): the substitution row 'abxd'
-- should match at max_cost 1.
SET enable_seqscan = off;
SELECT id, body FROM cost_t
WHERE body %~~ tre_pattern('abcd', 1)
ORDER BY id;
RESET enable_seqscan;

DROP TABLE cost_t CASCADE;
