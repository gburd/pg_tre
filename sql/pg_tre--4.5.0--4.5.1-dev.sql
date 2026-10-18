-- pg_tre 4.5.0 -> 4.5.1-dev upgrade.
--
-- This stub is filled in as catalog-changing features land in
-- the dev cycle.  At release time, run the audit from
-- RELEASING.md to confirm every new CREATE FUNCTION /
-- OPERATOR / OPERATOR CLASS / TYPE / CAST / ALTER OPERATOR
-- FAMILY in sql/pg_tre--4.5.1-dev.sql has a matching statement
-- here, and every removed/renamed object has a matching
-- DROP / ALTER.

-- 4.5.1: pg_tre_index_check pins search_path and qualifies every name
-- (pgspot PS001/PS005/PS016).  DROP + CREATE rather than CREATE OR
-- REPLACE, which could adopt a same-signature function someone else
-- created (pgspot PS002).
DROP FUNCTION pg_tre_index_check(regclass, int);
-- 4.5.0: pg_tre_index_check -- does the index return what a sequential
-- scan returns?  For patterns cut from the indexed column's own values it
-- counts matching rows by sequential scan, by bitmap index scan and by
-- plain index scan of THIS index, for each operator the opclass serves.
--
-- search_path is pinned (pg_catalog, then pg_temp) and every function,
-- operator and type is schema-qualified, so objects someone else creates
-- in a schema on the caller's search_path cannot take over the calls --
-- operators run this as superuser after upgrades.  Dynamic SQL names the
-- table by its regclass (schema-qualified under this search_path) and
-- pg_tre's own objects by @extschema@.
CREATE FUNCTION pg_tre_index_check(idx regclass, samples int DEFAULT 20)
    RETURNS TABLE(op text, pattern text, seq_rows bigint,
                  bitmap_rows bigint, indexscan_rows bigint, ok boolean)
    LANGUAGE plpgsql VOLATILE PARALLEL UNSAFE
    SET search_path = pg_catalog, pg_temp
    SET enable_seqscan = on
    SET enable_indexscan = on
    SET enable_bitmapscan = on
AS $fn$
DECLARE
    am       name;
    tbl      regclass;
    natts    int;
    attnum   int;
    col      text;
    pred     text;
    idxname  text;
    reltup   float8;
    pct      float8;
    v        text;
    lit      text;
    pre      text;
    re_lit   text;
    re_pre   text;
    like_lit text;
    q        text;
    plan     text;
    used     boolean;
    n_vals   int := 0;
    n_checks int := 0;
    n_bad    int := 0;
    n_unused int := 0;
    fmts     text;
    meta     text;
    ext      text := pg_catalog.quote_ident('@extschema@');
BEGIN
    SELECT a.amname, i.indrelid, i.indnatts, i.indkey[0],
           COALESCE(pg_catalog.pg_get_expr(i.indpred, i.indrelid), 'true'),
           c.relname
      INTO am, tbl, natts, attnum, pred, idxname
      FROM pg_catalog.pg_index i
      JOIN pg_catalog.pg_class c ON c.oid OPERATOR(pg_catalog.=) i.indexrelid
      JOIN pg_catalog.pg_am a ON a.oid OPERATOR(pg_catalog.=) c.relam
     WHERE i.indexrelid OPERATOR(pg_catalog.=) idx;
    IF am IS NULL OR am::text OPERATOR(pg_catalog.<>) 'tre' THEN
        RAISE EXCEPTION '"%" is not a tre index', idx;
    END IF;
    IF natts OPERATOR(pg_catalog.<>) 1 OR attnum OPERATOR(pg_catalog.=) 0 THEN
        RAISE EXCEPTION 'pg_tre_index_check supports indexes on a single plain column; "%" is on an expression', idx;
    END IF;
    IF samples OPERATOR(pg_catalog.<) 1 THEN
        RAISE EXCEPTION 'samples must be at least 1';
    END IF;

    -- The column, collated as the index is, so the planner can use it.
    SELECT CASE WHEN ic.indcollation[0] OPERATOR(pg_catalog.<>) 0::oid
                 AND ic.indcollation[0] OPERATOR(pg_catalog.<>) at.attcollation
                THEN pg_catalog.format('(%I COLLATE %I.%I)', at.attname, n.nspname, co.collname)
                ELSE pg_catalog.quote_ident(at.attname) END
      INTO col
      FROM pg_catalog.pg_index ic
      JOIN pg_catalog.pg_attribute at
        ON at.attrelid OPERATOR(pg_catalog.=) ic.indrelid
       AND at.attnum OPERATOR(pg_catalog.=) ic.indkey[0]
      LEFT JOIN pg_catalog.pg_collation co ON co.oid OPERATOR(pg_catalog.=) ic.indcollation[0]
      LEFT JOIN pg_catalog.pg_namespace n ON n.oid OPERATOR(pg_catalog.=) co.collnamespace
     WHERE ic.indexrelid OPERATOR(pg_catalog.=) idx;

    -- About 4x the wanted rows from a Bernoulli sample (all rows when small).
    SELECT c.reltuples INTO reltup FROM pg_catalog.pg_class c
     WHERE c.oid OPERATOR(pg_catalog.=) tbl;
    pct := CASE WHEN reltup OPERATOR(pg_catalog.<=) 0 THEN 100
                ELSE LEAST(100, GREATEST(0.001,
                         samples OPERATOR(pg_catalog.*) 400.0 OPERATOR(pg_catalog./) reltup)) END;

    FOR v IN EXECUTE pg_catalog.format(
        'SELECT %s::pg_catalog.text FROM %s TABLESAMPLE BERNOULLI (%s)'
        ' WHERE %s IS NOT NULL AND pg_catalog.length(%s::pg_catalog.text) OPERATOR(pg_catalog.>=) 3'
        ' AND (%s) LIMIT %s',
        col, tbl, pct, col, col, pred, samples)
    LOOP
        n_vals := n_vals OPERATOR(pg_catalog.+) 1;
        lit := pg_catalog.substr(v,
                   1 OPERATOR(pg_catalog.+) pg_catalog.floor(pg_catalog.random()
                       OPERATOR(pg_catalog.*) GREATEST(pg_catalog.length(v) OPERATOR(pg_catalog.-) 2, 1))::int,
                   3 OPERATOR(pg_catalog.+) pg_catalog.floor(pg_catalog.random() OPERATOR(pg_catalog.*) 6)::int);
        IF pg_catalog.length(lit) OPERATOR(pg_catalog.<) 3 THEN
            lit := pg_catalog.substr(v, 1, 3);
        END IF;
        pre := pg_catalog.substr(v, 1,
                   3 OPERATOR(pg_catalog.+) pg_catalog.floor(pg_catalog.random() OPERATOR(pg_catalog.*) 4)::int);
        re_lit := pg_catalog.regexp_replace(lit, '([.^$*+?(){}|\\\[\]-])', '\\\1', 'g');
        re_pre := pg_catalog.regexp_replace(pre, '([.^$*+?(){}|\\\[\]-])', '\\\1', 'g');
        like_lit := pg_catalog.replace(pg_catalog.replace(pg_catalog.replace(
                        lit, '\', '\\'), '%', '\%'), '_', '\_');

        FOR op, pattern, q IN
            SELECT c.o, c.p, c.w FROM (VALUES
                ('%~~ k=0', re_lit,
                 pg_catalog.format('%s OPERATOR(%s.%%~~) %s.tre_pattern(%L, 0)', col, ext, ext, re_lit)),
                ('%~~ k=1', re_lit,
                 CASE WHEN pg_catalog.length(lit) OPERATOR(pg_catalog.>=) 5
                      THEN pg_catalog.format('%s OPERATOR(%s.%%~~) %s.tre_pattern(%L, 1)', col, ext, ext, re_lit) END),
                ('~',     re_lit, pg_catalog.format('%s OPERATOR(pg_catalog.~) %L', col, re_lit)),
                ('~*',    pg_catalog.upper(re_lit),
                 pg_catalog.format('%s OPERATOR(pg_catalog.~*) %L', col, pg_catalog.upper(re_lit))),
                ('~ ^',   '^' OPERATOR(pg_catalog.||) re_pre,
                 pg_catalog.format('%s OPERATOR(pg_catalog.~) %L', col, '^' OPERATOR(pg_catalog.||) re_pre)),
                ('~* ^',  '^' OPERATOR(pg_catalog.||) pg_catalog.upper(re_pre),
                 pg_catalog.format('%s OPERATOR(pg_catalog.~*) %L', col,
                                   '^' OPERATOR(pg_catalog.||) pg_catalog.upper(re_pre))),
                ('LIKE',  pg_catalog.concat('%', like_lit, '%'),
                 pg_catalog.format('%s OPERATOR(pg_catalog.~~) %L', col, pg_catalog.concat('%', like_lit, '%'))),
                ('ILIKE', pg_catalog.concat('%', pg_catalog.upper(like_lit), '%'),
                 pg_catalog.format('%s OPERATOR(pg_catalog.~~*) %L', col,
                                   pg_catalog.concat('%', pg_catalog.upper(like_lit), '%')))) c(o, p, w)
             WHERE c.w IS NOT NULL
        LOOP
            n_checks := n_checks OPERATOR(pg_catalog.+) 1;
            q := pg_catalog.format('SELECT pg_catalog.count(*) FROM %s WHERE %s AND (%s)', tbl, q, pred);

            PERFORM pg_catalog.set_config('enable_seqscan', 'on', true);
            PERFORM pg_catalog.set_config('enable_indexscan', 'off', true);
            PERFORM pg_catalog.set_config('enable_bitmapscan', 'off', true);
            EXECUTE q INTO seq_rows;

            -- bitmap, then plain index scan; NULL when the plan does not use idx
            PERFORM pg_catalog.set_config('enable_seqscan', 'off', true);
            PERFORM pg_catalog.set_config('enable_bitmapscan', 'on', true);
            used := false;
            FOR plan IN EXECUTE pg_catalog.concat('EXPLAIN (COSTS OFF) ', q) LOOP
                used := used OR pg_catalog.strpos(plan,
                            pg_catalog.concat('Bitmap Index Scan on ', idxname)) OPERATOR(pg_catalog.>) 0;
            END LOOP;
            bitmap_rows := NULL;
            IF used THEN
                EXECUTE q INTO bitmap_rows;
            END IF;

            PERFORM pg_catalog.set_config('enable_bitmapscan', 'off', true);
            PERFORM pg_catalog.set_config('enable_indexscan', 'on', true);
            used := false;
            FOR plan IN EXECUTE pg_catalog.concat('EXPLAIN (COSTS OFF) ', q) LOOP
                used := used OR pg_catalog.strpos(plan,
                            pg_catalog.concat('Index Scan using ', idxname, ' ')) OPERATOR(pg_catalog.>) 0;
            END LOOP;
            indexscan_rows := NULL;
            IF used THEN
                EXECUTE q INTO indexscan_rows;
            END IF;

            IF bitmap_rows IS NULL AND indexscan_rows IS NULL THEN
                ok := NULL;
                n_unused := n_unused OPERATOR(pg_catalog.+) 1;
            ELSE
                ok := COALESCE(bitmap_rows OPERATOR(pg_catalog.=) seq_rows, true)
                      AND COALESCE(indexscan_rows OPERATOR(pg_catalog.=) seq_rows, true);
                IF NOT ok THEN
                    n_bad := n_bad OPERATOR(pg_catalog.+) 1;
                END IF;
            END IF;
            RETURN NEXT;
        END LOOP;
    END LOOP;

    EXECUTE pg_catalog.format(
        'SELECT pg_catalog.string_agg(pg_catalog.concat(''v'', format_version, '': '', page_count, '' pages''), '', '' ORDER BY format_version)'
        ' FROM %s.pg_tre_index_format_status($1)', ext)
        INTO fmts USING idx;
    EXECUTE pg_catalog.format(
        'SELECT pg_catalog.format(''TID packing %%s; %%s pending entries'', %s.pg_tre_index_tid_packing($1), %s.pg_tre_index_pending($1))',
        ext, ext)
        INTO meta USING idx;
    fmts := pg_catalog.concat('Page formats ', fmts, '; ', meta, '.',
                   CASE WHEN pg_catalog.strpos(fmts, ',') OPERATOR(pg_catalog.>) 0
                        THEN '  Mixed page formats are readable; REINDEX rewrites them all at the latest.'
                        ELSE '' END);
    IF n_bad OPERATOR(pg_catalog.>) 0 THEN
        RAISE WARNING 'pg_tre_index_check("%"): % of % checks disagree with a sequential scan',
            idx, n_bad, n_checks
            USING DETAIL = fmts,
                  HINT = 'The index returns different rows than a sequential scan: REINDEX INDEX CONCURRENTLY it, and please report the failing patterns.';
    ELSE
        RAISE NOTICE 'pg_tre_index_check("%"): % checks from % sampled values agree with a sequential scan (% could not use the index)',
            idx, n_checks, n_vals, n_unused
            USING DETAIL = fmts;
    END IF;
END
$fn$;

COMMENT ON FUNCTION pg_tre_index_check(regclass, int) IS
    'Compare the index with a sequential scan: for patterns cut from '
    '`samples` of the column''s own values, count matching rows by '
    'sequential scan, bitmap index scan and plain index scan for %~~ (k=0 '
    'and 1), ~, ~*, anchored ~ / ~*, LIKE and ILIKE.  One row per check; '
    'ok is false where the index disagrees, NULL where the planner did '
    'not use the index.  Costs about 8 sequential scans per sample.';
