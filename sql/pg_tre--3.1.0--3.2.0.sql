-- pg_tre 3.1.0 -> 3.2.0 migration.
--
-- 3.2.0 changes:
--   * REMOVED the BRIN-style range-bloom tier (it was built but never
--     consulted at scan time).  The pg_tre.range_size_blocks GUC and the
--     range_size_blocks reloption are retained but ignored (deprecated).
--   * ADDED a SuRF (Succinct Range Filter) tier over an order-preserving
--     trigram key (format v10) used to prefilter ^-anchored / prefix /
--     LIKE 'foo%' scans: a whole-index scan is skipped when no indexed
--     trigram falls in the anchored prefix's key range.
--
-- On-disk format bumps v9 -> v10 but stays backward-readable
-- (PG_TRE_FORMAT_VERSION_MIN = 6), so this UPDATE requires NO REINDEX:
-- an existing v6-v9 index reads unchanged and simply has no SuRF filter
-- (scans fall back to the normal posting-tier path).  To populate the
-- SuRF filter on an existing index, REINDEX it (or pg_tre_upgrade_index()
-- bumps the page stamps but does not synthesize the whole-index SuRF).

CREATE FUNCTION tre_surf_stats(regclass)
    RETURNS TABLE(n_keys bigint, n_nodes bigint,
                  n_pages int4, image_bytes bigint)
    AS 'MODULE_PATHNAME', 'tre_surf_stats'
    LANGUAGE C STRICT STABLE PARALLEL SAFE
    ROWS 1;

COMMENT ON FUNCTION tre_surf_stats(regclass) IS
    'SuRF range-filter (format v10) stats: distinct trigram-key count, '
    'trie node count, on-disk page count, and serialized image size.';
