-- test/sql/tid_packing.sql
-- 4.4.0 (format v11): dense TID packing (block * 292 + offset) versus the
-- legacy packing (block << 16 | offset) of every index built by 4.3 or
-- earlier.  pg_tre.build_legacy_tid_packing builds a legacy index; two
-- tables hold the same rows, one index each, so both read paths run on
-- the same data: bitmap scan, ordered scan (<@>, matched inside the AM),
-- the pending list, VACUUM, multi-leaf postings, heap blocks past 224
-- (65536 / 292), offsets up to 291, and REINDEX turning legacy into
-- dense.  Every check compares the index with a sequential scan.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_tre;
DROP TABLE IF EXISTS tp_l, tp_d CASCADE;
RESET client_min_messages;

-- 'row' is a multi-leaf posting, 'zq' a rare trigram.  The tail (ids
-- 80001-83000) is mostly NULL: 290 such tuples fit on a page, so offsets
-- reach the top of the dense stride (MaxHeapTuplesPerPage = 291).
CREATE TABLE tp_l (id int, s text) WITH (fillfactor = 100);
CREATE TABLE tp_d (id int, s text) WITH (fillfactor = 100);
INSERT INTO tp_l SELECT i, 'row' || (i % 997) || CASE WHEN i % 5000 = 7 THEN 'zq' ELSE '' END
  FROM generate_series(1, 60000) i;
INSERT INTO tp_l SELECT NULL, CASE WHEN i % 41 = 0 THEN 'tail' || (i % 7) END
  FROM generate_series(80001, 83000) i;
INSERT INTO tp_d SELECT * FROM tp_l;
SELECT max((ctid::text::point)[0]) > 224 AS past_block_224,
       max((ctid::text::point)[1]) >= 289 AS offsets_near_291,
       max((ctid::text::point)[1]) FILTER (WHERE s LIKE 'tail%') >= 280 AS tail_high
  FROM tp_d;

SET client_min_messages = warning;
SET pg_tre.build_legacy_tid_packing = on;
CREATE INDEX tp_l_idx ON tp_l USING tre (s);
RESET pg_tre.build_legacy_tid_packing;
CREATE INDEX tp_d_idx ON tp_d USING tre (s);
RESET client_min_messages;
SELECT pg_tre_index_tid_packing('tp_l_idx') AS legacy,
       pg_tre_index_tid_packing('tp_d_idx') AS dense;

-- seq scan vs bitmap scan vs ordered index scan on one table.
CREATE FUNCTION tp_check(tbl text, pat text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE seq_ids tid[]; bm_ids tid[]; ord_ids tid[];
BEGIN
  PERFORM set_config('enable_seqscan', 'on', true);
  PERFORM set_config('enable_indexscan', 'off', true);
  PERFORM set_config('enable_bitmapscan', 'off', true);
  EXECUTE format('SELECT coalesce(array_agg(ctid ORDER BY ctid), ''{}'') FROM %I
                   WHERE s %%~~ tre_pattern(%L, 0)', tbl, pat) INTO seq_ids;
  PERFORM set_config('enable_seqscan', 'off', true);
  PERFORM set_config('enable_bitmapscan', 'on', true);
  EXECUTE format('SELECT coalesce(array_agg(ctid ORDER BY ctid), ''{}'') FROM %I
                   WHERE s %%~~ tre_pattern(%L, 0)', tbl, pat) INTO bm_ids;
  PERFORM set_config('enable_bitmapscan', 'off', true);
  PERFORM set_config('enable_indexscan', 'on', true);
  EXECUTE format('SELECT coalesce(array_agg(ctid ORDER BY ctid), ''{}'') FROM
                   (SELECT ctid FROM %I WHERE s %%~~ tre_pattern(%L, 0)
                     ORDER BY s <@> tre_pattern(%L, 0) LIMIT 100000) q',
                 tbl, pat, pat) INTO ord_ids;
  RETURN CASE WHEN seq_ids = bm_ids AND seq_ids = ord_ids
              THEN 'ok ' || cardinality(seq_ids)
              ELSE format('BAD seq=%s bitmap=%s ordered=%s', cardinality(seq_ids),
                          cardinality(bm_ids), cardinality(ord_ids)) END;
END $$;

CREATE FUNCTION tp_all(tag text) RETURNS TABLE(tag text, pat text, legacy text, dense text)
LANGUAGE sql AS $$
  SELECT tag, p, tp_check('tp_l', p), tp_check('tp_d', p)
    FROM unnest(ARRAY['row', 'zq', 'row99[0-6]zq', 'w4', 'row123$', 'tail[36]', 'nomatch']) p
$$;

-- (1) Freshly built.
SELECT * FROM tp_all('built');

-- (2) Pending list: inserts land in each index's pending list, packed by
-- that index's packing, and are merged at scan time.
INSERT INTO tp_l SELECT i, 'row' || (i % 997) || 'zq' FROM generate_series(60001, 61000) i;
INSERT INTO tp_d SELECT i, 'row' || (i % 997) || 'zq' FROM generate_series(60001, 61000) i;
SELECT * FROM tp_all('pending');

-- (3) VACUUM: delete a slice of built and pending rows, then vacuum
-- (which also flushes the pending lists).
DELETE FROM tp_l WHERE id % 7 = 0 OR id BETWEEN 60500 AND 60600
                     OR (id IS NULL AND (ctid::text::point)[1]::int % 2 = 0);
DELETE FROM tp_d WHERE id % 7 = 0 OR id BETWEEN 60500 AND 60600
                     OR (id IS NULL AND (ctid::text::point)[1]::int % 2 = 0);
VACUUM tp_l;
VACUUM tp_d;
SELECT * FROM tp_all('vacuumed');

-- (4) New rows re-use the freed slots.
INSERT INTO tp_l SELECT i, 'row' || (i % 997) || 'zq' FROM generate_series(70001, 72000) i;
INSERT INTO tp_d SELECT i, 'row' || (i % 997) || 'zq' FROM generate_series(70001, 72000) i;
SELECT * FROM tp_all('refilled');

-- (5) REINDEX converts a legacy index.  Same rows, so the two indexes
-- are then the same size; before, the dense one is smaller.
SELECT pg_relation_size('tp_d_idx') < pg_relation_size('tp_l_idx') AS dense_smaller;
SET client_min_messages = warning;
REINDEX INDEX tp_l_idx;
REINDEX INDEX tp_d_idx;
RESET client_min_messages;
SELECT pg_tre_index_tid_packing('tp_l_idx') AS after_reindex,
       pg_relation_size('tp_l_idx') = pg_relation_size('tp_d_idx') AS same_size;
SELECT * FROM tp_all('reindexed');

-- (6) pg_tre_upgrade_index points a legacy index at REINDEX; quiet on a
-- dense one.
SET client_min_messages = warning;
SET pg_tre.build_legacy_tid_packing = on;
CREATE INDEX tp_l_idx2 ON tp_l USING tre (s);
RESET pg_tre.build_legacy_tid_packing;
RESET client_min_messages;
SELECT pg_tre_upgrade_index('tp_l_idx2');
SELECT pg_tre_upgrade_index('tp_d_idx');
DROP INDEX tp_l_idx2;

DROP FUNCTION tp_all(text);
DROP FUNCTION tp_check(text, text);
DROP TABLE tp_l, tp_d;
