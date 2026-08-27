-- test/sql/amvalidate.sql
-- Regression test: opclass validation (amvalidate) and KNN mark/restore.
--
-- amvalidate is invoked by the CREATE OPERATOR CLASS / ALTER OPERATOR
-- FAMILY machinery and by the amvalidate() diagnostic.  A correctly
-- defined tre opclass must validate cleanly (return true, no WARNING).

CREATE EXTENSION IF NOT EXISTS pg_tre;

-- The shipped tre_text_ops opclass must pass validation.
SELECT amvalidate(opc.oid)
FROM pg_opclass opc
JOIN pg_am am ON am.oid = opc.opcmethod
WHERE am.amname = 'tre' AND opc.opcname = 'tre_text_ops';

-- The opclass indexes text.
SELECT opcintype::regtype
FROM pg_opclass opc
JOIN pg_am am ON am.oid = opc.opcmethod
WHERE am.amname = 'tre' AND opc.opcname = 'tre_text_ops';

-- KNN mark/restore: exercise the ORDER BY <@> path under a merge join,
-- which is where the executor may call ammarkpos/amrestrpos over the
-- ordered amgettuple output.  The result must be correct regardless.
DROP TABLE IF EXISTS mr_t CASCADE;
CREATE TABLE mr_t (id serial, body text);
INSERT INTO mr_t (body)
SELECT 'connection refused code ' || (i % 20) FROM generate_series(1, 500) i;
CREATE INDEX mr_idx ON mr_t USING tre (body);

-- Nearest 5 by edit distance to 'connection refused'.
SET enable_seqscan = off;
SELECT body, body <@> tre_pattern('connection refused', 2) AS dist
FROM mr_t
WHERE body %~~ tre_pattern('connection refused', 2)
ORDER BY body <@> tre_pattern('connection refused', 2) ASC NULLS LAST, id
LIMIT 5;
RESET enable_seqscan;

DROP TABLE mr_t CASCADE;
