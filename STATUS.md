# pg_tre status

Live progress tracker for the 1.0.0 roadmap.  Keep this file
current on every commit.  See `doc/design.md` for the architecture
it tracks against.

Legend:  `[x]` done -- `[~]` partial / stubbed -- `[ ]` not started

## Phase 0 -- Foundation

- [x] Delete committed build artifacts (pg_tre.dylib, *.o).
- [x] Submodules: vendor/tre (pinned v0.9.0), vendor/lime (main).
- [x] New source-tree layout (src/am, src/pages, src/wal,
  src/query, src/util, include/pg_tre).
- [x] Makefile drives TRE autotools + sparsemap + PGXS;
  Lime codegen rule present but not yet wired (grammar source
  exists but not compiled because extract.c is still a stub).
- [x] PG18+ version guard.
- [x] Licenses: LICENSE (MIT), NOTICE covering TRE + Lime + sparsemap.
- [x] `.gitignore`, `.github/workflows/ci.yml`, `scripts/run-regress.sh`.
- [x] Legacy UDFs preserved; regression test green.
- [x] `CREATE ACCESS METHOD tre` registers.
- [x] `CREATE INDEX ... USING tre` parses and creates an empty
  stub index.

## Phase 1 -- On-disk format & WAL

- [x] Page layout declarations (include/pg_tre/page.h).
- [x] WAL record declarations + rmgr registration glue
  (include/pg_tre/xlog.h, src/wal/xlog.c).
- [x] RmgrId 140 registered only when preload-loaded.
- [ ] Meta page read/write (src/pages/meta.c bodies).
- [ ] Upper-tree Lehman-Yao page split (src/pages/upper.c bodies).
- [ ] Posting-leaf sparsemap wrap / unwrap with in-place edits
  (src/pages/posting.c bodies).
- [ ] WAL redo bodies for META_UPDATE and the empty-page init
  records.
- [ ] TAP test for crash recovery of an empty index
  (test/t/001_crash_empty.pl).
- [ ] Phase-1 exit gate: an empty index survives `pg_ctl stop -m
  immediate` and replays cleanly.

## Phase 2 -- Build path

- [ ] Tuplesort-backed ambuild.
- [ ] Upper-tree and posting-tree bulk loaders.
- [ ] Parallel build (amcanbuildparallel=true).
- [ ] Regression: an index on a 10k-row fixture builds without
  error and `pg_relation_size` is within expected bounds.

## Phase 3 -- Scan path, exact regex (k=0)

- [ ] Lime codegen wired into build; tre_grammar.c / tre_grammar.h
  generated and compiled.
- [ ] Hand-written tokenizer src/query/tokens.c.
- [ ] Regex AST + k=0 extraction (src/query/extract.c).
- [ ] amgetbitmap pulls posting lists via sparsemap_intersection.
- [ ] tre_pattern type (in/out/recv/send) and %~~ operator.
- [ ] Differential test: 10k-row corpus x 100 regex patterns, index
  scan == seq scan.

## Phase 4 -- Incremental writes

- [ ] Pending-list aminsert append.
- [ ] VACUUM bulkdelete + cleanup merge.
- [ ] pg_tre_flush() function.
- [ ] fastupdate storage option.
- [ ] HOT-style sustained-write regression passes.

## Phase 5 -- Approximate regex & three-tier funnel

- [ ] Extraction with edit budget, `{~m}` support, Navarro tiling.
- [ ] Per-tuple bloom filter payload in posting leaves.
- [ ] BRIN-style range summary tier.
- [ ] Position-aware tier-2 filtering (sparsemap_offset).
- [ ] Universal-Levenshtein small-k expansion.
- [ ] Differential tests at k in {0,1,2,3}, pattern lengths 3..40.

## Phase 6 -- Planner & DoS hardening

- [ ] amcostestimate using metapage-cached per-trigram cardinalities.
- [ ] tre_pattern_sel restriction selectivity.
- [ ] amoptions (q, range_size, bloom_bits, fastupdate).
- [ ] amvalidate.
- [ ] NFA state cap + compile/match timeouts.

## Phase 7 -- Durability, replicas, VACUUM

- [ ] Streaming replica tests.
- [ ] REINDEX CONCURRENTLY.
- [ ] VACUUM FULL / CLUSTER TID remap.
- [ ] pg_upgrade compat test.
- [ ] Long soak with CLOBBER_CACHE_ALWAYS.
- [ ] wal_consistency_checking='pg_tre' leg in CI.

## Phase 8 -- Performance tuning

- [ ] ReadStream-based posting prefetch.
- [ ] Parallel index scan.
- [ ] Published benchmarks in bench/.
- [ ] Optional WAL deltas for sparsemap edits.

## Phase 9 -- Docs & release

- [ ] Full user docs (doc/pg_tre.md).
- [ ] Migration guide 0.1.0 -> 1.0.0.
- [ ] PGXN META.json, Homebrew tap, Debian/RPM templates.
- [ ] Release checklist executed; 1.0.0 tag.

## Known issues today

- pg_regress on this nix-built toolchain fails to exec `sh`;
  `scripts/run-regress.sh` works around by calling `psql`
  directly.  The GitHub Actions CI matrix uses a stock
  apt-installed PostgreSQL where pg_regress works and `make
  installcheck` can be used instead.
- GUCs `pg_tre.range_size_blocks` and `pg_tre.bloom_tuple_bits`
  are currently SIGHUP-level; Phase 6 will move these to
  per-index reloptions where they belong.
