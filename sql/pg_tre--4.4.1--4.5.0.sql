-- pg_tre 4.4.1 -> 4.5.0 upgrade.
--
-- This stub is filled in as catalog-changing features land in
-- the dev cycle.  At release time, run the audit from
-- RELEASING.md to confirm every new CREATE FUNCTION /
-- OPERATOR / OPERATOR CLASS / TYPE / CAST / ALTER OPERATOR
-- FAMILY in sql/pg_tre--4.5.0.sql has a matching statement
-- here, and every removed/renamed object has a matching
-- DROP / ALTER.

-- 4.5.0: pg_tre_index_pending
CREATE FUNCTION pg_tre_index_pending(regclass)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'pg_tre_index_pending'
    LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

COMMENT ON FUNCTION pg_tre_index_pending(regclass) IS
    'Entries (24 bytes each) in the index''s pending list, which every '
    'scan reads in full.  VACUUM merges the list, as does an INSERT that '
    'pushes it past pending_list_limit.';

-- 4.5.0: pg_tre_index_check -- does the index return what a sequential
-- scan returns?  For patterns cut from the indexed column's own values it
-- counts matching rows by sequential scan, by bitmap index scan and by
-- plain index scan of THIS index, for each operator the opclass serves.
CREATE FUNCTION pg_tre_index_check(idx regclass, samples int DEFAULT 20)
    RETURNS TABLE(op text, pattern text, seq_rows bigint,
                  bitmap_rows bigint, indexscan_rows bigint, ok boolean)
    LANGUAGE plpgsql VOLATILE PARALLEL UNSAFE
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
BEGIN
    SELECT a.amname, i.indrelid, i.indnatts, i.indkey[0],
           coalesce(pg_get_expr(i.indpred, i.indrelid), 'true'), c.relname
      INTO am, tbl, natts, attnum, pred, idxname
      FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
      JOIN pg_am a ON a.oid = c.relam
     WHERE i.indexrelid = idx;
    IF am IS DISTINCT FROM 'tre' THEN
        RAISE EXCEPTION '"%" is not a tre index', idx;
    END IF;
    IF natts <> 1 OR attnum = 0 THEN
        RAISE EXCEPTION 'pg_tre_index_check supports indexes on a single plain column; "%" is on an expression', idx;
    END IF;
    IF samples < 1 THEN
        RAISE EXCEPTION 'samples must be at least 1';
    END IF;

    -- The column, collated as the index is, so the planner can use it.
    SELECT CASE WHEN ic.indcollation[0] <> 0 AND ic.indcollation[0] <> at.attcollation
                THEN format('(%I COLLATE %I.%I)', at.attname, n.nspname, co.collname)
                ELSE quote_ident(at.attname) END
      INTO col
      FROM pg_index ic
      JOIN pg_attribute at ON at.attrelid = ic.indrelid AND at.attnum = ic.indkey[0]
      LEFT JOIN pg_collation co ON co.oid = ic.indcollation[0]
      LEFT JOIN pg_namespace n ON n.oid = co.collnamespace
     WHERE ic.indexrelid = idx;

    -- About 4x the wanted rows from a Bernoulli sample (all rows when small).
    SELECT reltuples INTO reltup FROM pg_class WHERE oid = tbl;
    pct := CASE WHEN reltup <= 0 THEN 100
                ELSE least(100, greatest(0.001, samples * 400.0 / reltup)) END;

    FOR v IN EXECUTE format(
        'SELECT %s::text FROM %s TABLESAMPLE BERNOULLI (%s)'
        ' WHERE %s IS NOT NULL AND length(%s::text) >= 3 AND (%s) LIMIT %s',
        col, tbl, pct, col, col, pred, samples)
    LOOP
        n_vals := n_vals + 1;
        lit := substr(v, 1 + floor(random() * greatest(length(v) - 2, 1))::int,
                      3 + floor(random() * 6)::int);
        IF length(lit) < 3 THEN
            lit := substr(v, 1, 3);
        END IF;
        pre := substr(v, 1, 3 + floor(random() * 4)::int);
        re_lit := regexp_replace(lit, '([.^$*+?(){}|\\\[\]-])', '\\\1', 'g');
        re_pre := regexp_replace(pre, '([.^$*+?(){}|\\\[\]-])', '\\\1', 'g');
        like_lit := replace(replace(replace(lit, '\', '\\'), '%', '\%'), '_', '\_');

        FOR op, pattern, q IN
            SELECT * FROM (VALUES
                ('%~~ k=0', re_lit, format('%s %%~~ tre_pattern(%L, 0)', col, re_lit)),
                ('%~~ k=1', re_lit, CASE WHEN length(lit) >= 5
                                         THEN format('%s %%~~ tre_pattern(%L, 1)', col, re_lit) END),
                ('~',       re_lit, format('%s ~ %L', col, re_lit)),
                ('~*',      upper(re_lit), format('%s ~* %L', col, upper(re_lit))),
                ('~ ^',     '^' || re_pre, format('%s ~ %L', col, '^' || re_pre)),
                ('~* ^',    '^' || upper(re_pre), format('%s ~* %L', col, '^' || upper(re_pre))),
                ('LIKE',    '%' || like_lit || '%', format('%s LIKE %L', col, '%' || like_lit || '%')),
                ('ILIKE',   '%' || upper(like_lit) || '%',
                            format('%s ILIKE %L', col, '%' || upper(like_lit) || '%'))) c(o, p, w)
             WHERE w IS NOT NULL
        LOOP
            n_checks := n_checks + 1;
            q := format('SELECT count(*) FROM %s WHERE %s AND (%s)', tbl, q, pred);

            PERFORM set_config('enable_seqscan', 'on', true);
            PERFORM set_config('enable_indexscan', 'off', true);
            PERFORM set_config('enable_bitmapscan', 'off', true);
            EXECUTE q INTO seq_rows;

            -- bitmap, then plain index scan; NULL when the plan does not use idx
            PERFORM set_config('enable_seqscan', 'off', true);
            PERFORM set_config('enable_bitmapscan', 'on', true);
            used := false;
            FOR plan IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
                used := used OR plan LIKE '%Bitmap Index Scan on ' || idxname || '%';
            END LOOP;
            bitmap_rows := NULL;
            IF used THEN
                EXECUTE q INTO bitmap_rows;
            END IF;

            PERFORM set_config('enable_bitmapscan', 'off', true);
            PERFORM set_config('enable_indexscan', 'on', true);
            used := false;
            FOR plan IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
                used := used OR plan LIKE '%Index Scan using ' || idxname || ' %';
            END LOOP;
            indexscan_rows := NULL;
            IF used THEN
                EXECUTE q INTO indexscan_rows;
            END IF;

            IF bitmap_rows IS NULL AND indexscan_rows IS NULL THEN
                ok := NULL;
                n_unused := n_unused + 1;
            ELSE
                ok := coalesce(bitmap_rows = seq_rows, true)
                      AND coalesce(indexscan_rows = seq_rows, true);
                IF NOT ok THEN
                    n_bad := n_bad + 1;
                END IF;
            END IF;
            RETURN NEXT;
        END LOOP;
    END LOOP;

    SELECT string_agg('v' || format_version || ': ' || page_count || ' pages', ', '
                      ORDER BY format_version)
      INTO fmts FROM pg_tre_index_format_status(idx);
    fmts := format('Page formats %s; TID packing %s; %s pending entries.%s',
                   fmts, pg_tre_index_tid_packing(idx), pg_tre_index_pending(idx),
                   CASE WHEN fmts LIKE '%,%'
                        THEN '  Mixed page formats are readable; REINDEX rewrites them all at the latest.'
                        ELSE '' END);
    IF n_bad > 0 THEN
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
