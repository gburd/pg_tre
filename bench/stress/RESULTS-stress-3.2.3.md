# pg_tre 3.2.3 qualification — results

Run: `bench/stress/stress-suite.sh` on AWS **i4i.8xlarge** (32 vCPU, 256 GB
RAM, 2x3.75 TB Nitro NVMe RAID-0 = 6.9 TB on `/mnt/nvme`), Amazon Linux
2023, PostgreSQL 18.0 (`-O2`, no cassert), pg_tre @ **v3.2.3** built from a
clean recursive clone of the tag (submodules at their pinned revs), vendored
TRE at upstream master `f864ed0`. Corpus: `gen_large_corpus.py`, 1M rows,
`medium` shape, planted tokens (government 5% / electrification 1% /
naturalize 0.1% / absent) plus a 1% anchored-`^government` fraction.

3.2.3 fixes an index read-path bug (the v10 SuRF prefilter rejected
case-insensitive anchored patterns and returned zero rows) and bumps
vendored TRE. The gates below are therefore ordered with the **reported
bug** first: a fix nobody verified against the actual reported scenario is
not qualified, however green the rest of the suite is.

## Verdict: QUALIFIED

### Gate 1 — the reported bug, against a real 3.2.2-built index

The strongest available check is not a synthetic unit test but the
reporter's own scenario: build the index with the **actual released 3.2.2**
(cloned from Codeberg at tag `v3.2.2`, built on the box), confirm the bug is
live, then load the 3.2.3 `.so` and `ALTER EXTENSION ... UPDATE` **with no
REINDEX** against that same on-disk index.

| query | 3.2.2 index scan | 3.2.2 seq scan | 3.2.3 index scan | 3.2.3 seq scan |
|-------|-----------------:|---------------:|-----------------:|---------------:|
| `name ~* '^GIT'`   | **0  ← BUG** | 3 | **3** | 3 |
| `name ~* '^git'`   | 3 | 3 | 3 | 3 |
| `name ILIKE 'GIT%'`| 3 | 3 | 3 | 3 |
| `name ~ '^git'`    | 3 | 3 | 3 | 3 |

Confirmed by `pg_tre_upgrade_index`-free, `REINDEX`-free upgrade: loading
the new `.so` is what corrects the answers, exactly as the release note
claims. Writes into the still-3.2.2-format index afterwards remain correct
(insert 50 + delete 1 against 3 planted rows -> index and seq scan both
report 52).

Note `ILIKE 'GIT%'` did **not** reproduce the failure on 3.2.2 — its
extraction reaches the prefilter differently. Worth recording because it
means an operator smoke-testing only with `ILIKE` would have seen a clean
bill of health on a broken build.

### Gate 2 — case-insensitive correctness at scale (1M rows)

Every case-insensitive anchored shape, index vs sequential ground truth:

| query | index | seq scan |
|-------|------:|---------:|
| `body ~* '^GOVERNMENT'`   | 71,968 | 71,968 |
| `body ~* '^GoVeRnMeNt'`   | 71,968 | 71,968 |
| `body ILIKE 'GOVERNMENT%'`| 71,968 | 71,968 |
| `body ~* '^ZZQQXJWK'` (genuinely absent) | 0 | 0 |

The absent case still returning 0 matters: the fix must not have been "stop
filtering and return everything".

### Gate 3 — the acceleration is preserved, not disabled

The lazy fix for a bad filter is to switch the filter off. That would have
passed every correctness gate above and silently cost the feature 3.2.0 was
released for. Scenario J measures it directly:

- **anchored-absent reject p50 = 0.060 ms** at 1M rows — unchanged from the
  3.2.2 baseline (0.060 ms) and still O(1) in table size.
- `DEBUG1` confirms case-*sensitive* absent prefixes still take the
  short-circuit ("SuRF prefilter rejected scan").

### Gate 4 — build, regression, and the standard scenarios

| gate | result |
|------|--------|
| clean build from the tag | **zero warnings** |
| TRE progress-hook patch | applied at **10 sites**, verified present post-build |
| `tre_version()` | `pg_tre 3.2.3 (TRE 0.9.0)` — now read from the library at runtime |
| regression suite | **41/41** |
| C — cold-cache (index 1275 MB > SB 4 GB working set) | **PASS** — 0 mismatches on all 8 queries, cold and warm |
| D — CIC under concurrent writes | **PASS** — `indisvalid=t`, 0 oracle mismatches |
| E — cancellation mid-build | **PASS** — honored in 0.07 s, no valid half-built index |
| G — VACUUM under churn | **PASS on correctness** (0 mismatches); reproduces the known 3.2.x bloat finding (3.87x) |
| H — pathological patterns | **PASS** — compile bomb rejected in 0.00 s, high-k bounded, timeout effective |
| I — parallel saturation | **PASS** — parallel 418.8 s vs serial 521.5 s, **identical hits (59,167)**, 0 stuck spinlocks |
| J — SuRF at scale | **PASS** — see Gate 3 |

### Gate 5 — vendored TRE, upstream suite + DoS guards

`patches/tre-progress-hook.patch` **is** pg_tre's compile/match timeout
enforcement, and the TRE bump rewrote all four files it touches. A rebase
that quietly orphaned a hook would defang `pg_tre.compile_timeout_ms` while
leaving every test above green, so the hooks were counted, not assumed:

| check | result |
|-------|--------|
| compile hook reached | 719 calls |
| compile abort honored | `rc=12` (REG_ESPACE) on `((a{50}){50}){50}` |
| match hook, parallel matcher | 200,000 calls (one per input position) |
| match hook, approx matcher | 200,000 calls |
| match abort short-circuits | 1 call vs 200,000 |
| upstream TRE suite with our patch applied | **107,091/107,091**, no leaks |

All ten hook sites confirmed still inside their intended functions (the
AST-expansion loops and all three matcher driver loops).

## Not re-run, and why

- **Scenario B** (`maintenance_work_mem` starvation): at 10M rows with 64 MB
  mwm this is the documented >20 min / no-finish build wall from
  `RESULTS-stress-3.2.0.md`. It carries no accuracy oracle beyond C and D
  and re-proves a performance finding, not anything this release touches.
- **Scenario F** (SIGKILL mid-build): passed in 3.2.2 and its harness defect
  is fixed (see below); nothing in 3.2.3 touches WAL or recovery. Skipped to
  keep the rig short-lived.
- **The 10M-row matrix**: started, then abandoned — a single 10M build
  exceeds 20 min and the scenario rebuilds per case. The 3.2.2 run already
  established 10M cold-cache behaviour with 0 mismatches, and nothing in
  this release changes page decode. 1M is where the *new* risk (the
  prefilter) is fully exercised.

## Pre-existing findings (reproduced, not regressions)

Unchanged from `RESULTS-stress-3.2.0.md` / `-3.2.2.md`, both documented in
`LIMITATIONS.md`, neither related to this release:

1. **Super-linear build throughput at scale** — 1M medium-shape build is
   ~419 s parallel / ~522 s serial; the leader-serial posting/upper/SuRF
   phase dominates.
2. **Posting-leaf bloat under sustained churn** — scenario G grows
   1082 MB -> 4192 MB (3.87x) over six delete+reinsert rounds with VACUUM
   each round; REINDEX fully reclaims. Correctness unaffected (0
   mismatches).

## Harness note

The scenario-F orphan defect found during the 3.2.2 run is fixed in this
tree: cleanup now kills by process *group* (a parallel worker retitles to
`postgres: ... CREATE INDEX` and carries no `$PGDATA`, so the old
name-matching `pkill` missed exactly the children holding the shm segment),
and F is ordered last so a cluster it fails to restart cannot strand the
scenarios after it.
