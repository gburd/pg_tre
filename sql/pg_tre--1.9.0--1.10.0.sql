-- pg_tre 1.9.0 -> 1.10.0 upgrade.
--
-- Phase A (A1): LIKE / ILIKE / ~ / ~* / = index acceleration.
--
-- Bind PostgreSQL's built-in text pattern operators into the pg_tre
-- operator family so the planner will use a pg_tre index for
-- col LIKE '%foo%', col ~ 'fo+o', col = 'foo', etc.  Each lowers to
-- the existing trigram engine at k=0; the executor rechecks with the
-- built-in operator, so the index is a lossy candidate filter and
-- correctness is unchanged.
--
-- No on-disk format change; no REINDEX.
--
-- Strategy numbers (must match include/pg_tre/amapi.h):
--   3 = ~~   (LIKE)     7 = =   (equality)
--   4 = ~~*  (ILIKE)
--   5 = ~    (regex)
--   6 = ~*   (iregex)

ALTER OPERATOR FAMILY tre_text_ops USING tre ADD
    OPERATOR 3 ~~  (text, text),
    OPERATOR 4 ~~* (text, text),
    OPERATOR 5 ~   (text, text),
    OPERATOR 6 ~*  (text, text),
    OPERATOR 7 =   (text, text);

COMMENT ON OPERATOR FAMILY tre_text_ops USING tre IS
    'pg_tre text operators: %~~ (approx regex), <@> (edit distance, '
    'ORDER BY), and LIKE/ILIKE/regex/equality acceleration.';
