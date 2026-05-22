# pg_tre status

Released: **1.1.1** (2026-05).  Currently in flight: **1.2.0-dev**
project-infrastructure cycle.  See `CHANGELOG.md` for the full
release notes and `doc/design.md` for the architecture this
file tracks against.

1.2.0-dev is a between-release cycle bringing pg_tre's project
infrastructure up to the bar set by `pg_textsearch`.  No source-
code behavior changes; everything is in CI workflows, release
engineering, code-quality tooling, and documentation.  See
`RELEASING.md`, `CONTRIBUTING.md`, `SECURITY.md`,
`.github/workflows/{upgrade-tests,pgspot,formatting}.yml`,
`scripts/bump-version.sh`.  Deferred Tier-3 followups are
tracked in `.agent/notes/community-readiness-followups.md`.

1.1.1 was a hardening release on the 1.1.0 lineage: vendored
sparsemap 2.2.0 → 2.3.0 (defensive bounds checks against
corrupt input), no on-disk format changes, no re-index
required.

1.1.0 was a maintenance release on the 1.0.0 lineage: same
on-disk format, same SQL surface, no re-index required.
It picked up sparsemap 2.2.0 and fixed the multi-leaf
right-link `sm_union` reversed-logic bug that silently
dropped every leaf past the first.

## What ships in 1.0.0

### Storage and recovery
- Custom IndexAmRoutine registered as `USING tre`.
- On-disk format v3: meta page, upper tree, multi-leaf
  posting trees with Lehman-Yao right-links, pending list,
  range-bloom tier, payload region.
- Custom rmgr (id 140) with full WAL coverage; crash
  recovery and streaming replication validated by TAP tests
  in `tap/`.

### Query path
- `body %~~ tre_pattern(P, k)` operator drives an indexed
  bitmap heap scan.
- Lime LALR(1) regex parser.
- Russ-Cox-style trigram extraction at k=0; Navarro tiling
  + universal-Levenshtein expansion at k>0.
- Three-tier filter funnel: range bloom → trigram postings
  → per-tuple bloom → TRE recheck.
- DNF / CNF dispatch in the AM driver.
- Pending-list overlay so newly-inserted rows are visible
  to scans before vacuum flushes them to posting trees.

### Operations
- Planner cost model with selectivity backed by index meta
  statistics.
- Per-pattern compile and match timeouts; NFA state cap;
  trigram-extraction fanout cap.  All exposed as GUCs.
- Reloptions: `bloom_tuple_bits`, `range_size_blocks`,
  `fastupdate`, `tuple_bloom_enable`, `pending_list_limit`.
- Legacy `tre_amatch*` UDFs from 0.1.0 preserved.

### Verification
- 12 differential regression tests (every k, every pattern
  shape: `seq-scan via tre_amatch == index-scan via %~~`).
- Three TAP tests under `tap/` for concurrency, streaming
  replication, and crash recovery.
- libFuzzer harness under `fuzz/` for the regex parser.

### Packaging and CI
- `META.json` for PGXN.
- `debian/`, `packaging/pg_tre.spec` for distro packages.
- `doc/Dockerfile` and `doc/homebrew-formula.rb` templates.
- `make dist` for source tarballs.
- GitHub Actions and Forgejo Actions (Codeberg) CI.
- Dependabot (GitHub) and Renovate (Codeberg) for
  dependency updates.

## v1.2 followups

These are real engineering deliverables, not bugs in 1.1.0.

- Tier-3 per-tuple bloom and positional filter across
  multi-leaf chains.  The payload offset for both filters is
  computed from a per-leaf rank that does not accumulate
  across right-link chains; until that lookup is repaired,
  `pg_tre.tuple_bloom_enable` bypasses both filters.
  Executor recheck stays authoritative for correctness; the
  filters are CPU optimizations.
- Migrate the remaining `sm_create + sm_add` build-path
  loops to `sm_add_grow` and the in-place set-op variants
  (`sm_union_inplace` etc.) added in sparsemap 2.0/2.2.
- PostgreSQL palloc allocator hook.  Sparsemap 2.2.0 added
  `sm_set_allocator` / `sm_create_with_allocator`; routing
  every sparsemap allocation through palloc + memory
  contexts would let pg_tre delete its explicit `sm_free`
  call-sites and let context teardown handle lifetime.
  The threading question (build-time vs query-time vs DSM)
  needs design work first.
- Parallel index scan (`amcanparallel = true`).  Bitmap heap
  scans built from a pg_tre index already parallelize on
  the heap side; AM-side parallelism is the gap.
- DNF positional filter.  Per-tuple position checking
  applies to CNF (k=0) only today; the tiled k≥1 path skips
  it, since tile alternative position windows are widened
  beyond what per-TID filtering can cheaply exploit.
  Recheck preserves correctness; this is a CPU optimization.
- Per-index reloptions for the SIGHUP-only GUCs
  (`range_size_blocks`, `bloom_tuple_bits`).
- 1M-row real-corpus benchmark execution + `doc/perf.md`
  refresh.
- libFuzzer harness fidelity: replace stub structs in
  `fuzz/memutils_stub.c` with real pg_tre header includes
  so the campaign runs without false ASAN reports against
  the harness itself.
- ≥30-day external beta on a non-trivial workload.

## Build and test

```
PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config
make           PG_CONFIG=$PG_CONFIG
make install   PG_CONFIG=$PG_CONFIG
PG_CONFIG=$PG_CONFIG bash scripts/run-regress.sh
```

Pre-tag gate: `scripts/release-check.sh`.
