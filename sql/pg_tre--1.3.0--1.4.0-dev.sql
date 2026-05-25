-- pg_tre upgrade 1.3.0 -> 1.4.0-dev
--
-- Adds index-side ORDER BY <@> support:
--   * Strategy 2 of tre_text_ops becomes the ordering operator
--     <@> (text, tre_pattern), returning int4 edit distance.
--   * Combined with amcanorderbyop=true in the AM handler and a new
--     amgettuple implementation, the planner can answer
--       SELECT ... WHERE body %~~ pat ORDER BY body <@> pat LIMIT N
--     directly from the index, in ascending distance order, without
--     materialising the full result set into a Sort node.
--
-- The on-disk index format is unchanged.  Existing indexes do not
-- need REINDEX; only the catalog is touched.

ALTER OPERATOR FAMILY tre_text_ops USING tre ADD
    OPERATOR 2 <@> (text, tre_pattern) FOR ORDER BY integer_ops;
