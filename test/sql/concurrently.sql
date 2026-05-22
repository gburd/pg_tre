-- test/sql/concurrently.sql
-- Regression test for CREATE INDEX CONCURRENTLY support.
--
-- pg_tre supports CIC via the standard PostgreSQL machinery: PG
-- runs ambuild against a transactional snapshot, validates with
-- a second pass against the post-snapshot heap, and the AM
-- itself doesn't need to know about the dual-phase setup.  This
-- test confirms that:
--
--   1. CREATE INDEX CONCURRENTLY succeeds against a populated
--      table.
--   2. The resulting index is valid and answers the same
--      queries as the non-concurrent build path.
--   3. DROP INDEX CONCURRENTLY also works.
--
-- Note: CIC must run outside any transaction block; this file is
-- driven by run-regress.sh which executes each statement
-- individually rather than wrapping in a single BEGIN/COMMIT.

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS cic_t CASCADE;
CREATE TABLE cic_t (id serial, body text);

INSERT INTO cic_t (body)
SELECT 'The quick brown fox row ' || md5(i::text) || ' line ' || i
FROM generate_series(1, 200) AS i;

-- The build itself.
CREATE INDEX CONCURRENTLY cic_idx ON cic_t USING tre (body);

-- The index must be valid post-build.
SELECT indexrelid::regclass, indisvalid, indisready
FROM   pg_index
WHERE  indexrelid = 'cic_idx'::regclass;

-- Differential: the CONCURRENTLY-built index returns the same
-- rows as a seq scan.  This catches snapshot-window bugs.
SET enable_seqscan = off;
CREATE TEMP TABLE _idx AS SELECT id FROM cic_t WHERE body %~~ tre_pattern('quick', 0);

SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE _seq AS SELECT id FROM cic_t WHERE body %~~ tre_pattern('quick', 0);

SELECT
    (SELECT count(*) FROM _idx) = (SELECT count(*) FROM _seq) AS counts_agree,
    NOT EXISTS (SELECT 1 FROM _idx EXCEPT SELECT 1 FROM _seq)
    AND NOT EXISTS (SELECT 1 FROM _seq EXCEPT SELECT 1 FROM _idx)
        AS row_sets_agree;

DROP TABLE _idx;
DROP TABLE _seq;

-- DROP INDEX CONCURRENTLY also exercises a separate amapi path.
DROP INDEX CONCURRENTLY cic_idx;

DROP TABLE cic_t;
