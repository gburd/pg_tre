# pg_tre at-scale stress harness

Adverse-conditions, at-scale, under-load testing for pg_tre on a large EC2
instance with **local NVMe**. This is the heavy sibling of `bench/ab-bench.sh`
(which is a 50k-row functional/latency A/B). Read `STRESS-PLAN.md` first — it
defines the scenarios (A–J), the instance/storage choices, and the pass/fail
criteria.

## TL;DR end-to-end

```bash
# --- on your workstation ---
export AWS_PROFILE=mala REGION=us-east-2
./provision-ec2.sh launch                 # launches i4i.8xlarge, stripes NVMe, tunes OS
# note the 'ssh -F <cfg> stress' alias it prints, and the TERMINATE command

# build + install PostgreSQL 18 on the box (optimized, no cassert):
ssh -F /tmp/pgtre-stress-sshcfg stress '
  curl -sO https://ftp.postgresql.org/pub/source/v18.0/postgresql-18.0.tar.bz2 &&
  tar xf postgresql-18.0.tar.bz2 && cd postgresql-18.0 &&
  ./configure --prefix=$HOME/pg18 CFLAGS=-O2 >/dev/null && make -j"$(nproc)" >/dev/null && make install >/dev/null &&
  (cd contrib/pg_trgm && make >/dev/null && make install >/dev/null)'

# ship the pg_tre source (this checkout) up:
tar czf /tmp/pgtre.tgz --exclude=.git --exclude='*.o' --exclude='*.so' -C .. pg_tre
scp -F /tmp/pgtre-stress-sshcfg /tmp/pgtre.tgz stress:~/
ssh -F /tmp/pgtre-stress-sshcfg stress 'tar xzf pgtre.tgz'

# --- on the box (over SSH) ---
ssh -F /tmp/pgtre-stress-sshcfg stress
  cd ~/pg_tre/bench/stress
  ./stress-suite.sh build-stack --src ~/pg_tre
  ./stress-suite.sh init
  ./stress-suite.sh load --rows 10000000 --shape medium
  ./stress-suite.sh run --only A,B,C,F,H,I,J
  cat ~/stress-results/*/SUMMARY.md

# --- back on your workstation: PULL RESULTS, THEN TERMINATE ---
scp -F /tmp/pgtre-stress-sshcfg -r stress:~/stress-results ./
./provision-ec2.sh terminate              # <-- do not skip this
```

## Scaling knobs

`gen_large_corpus.py --rows N --shape {short|medium|long}` sets the two
independent cost axes:

| shape  | ~row width | recommended max rows (i4i.8xlarge, 256 GB) |
|--------|-----------:|-------------------------------------------:|
| short  | ~50 chars  | 100M (heap ~6 GB, tests tid-bloom + SuRF scale) |
| medium | ~400 chars | 10–20M (the default working point)          |
| long   | ~2–4 KB    | 1–5M (hits the build temp-disk wall first)  |

`stress-suite.sh` env: `SHARED_BUFFERS` (keep it SMALL vs the dataset for
scenario C to be meaningful), `MWM`, `RUNS`, `NVME`, `PGBIN`.

## Scenarios (`--only A,B,...`)

| id | name | probes |
|----|------|--------|
| A | temp-disk exhaustion | clean cancellable build failure, not PANIC |
| B | mwm starvation | bounded RSS; build completes via disk spill |
| C | cold-cache queries | latency when index ≫ shared_buffers |
| D | CIC under writes | concurrent-build correctness (oracle) |
| E | cancellation mid-build | cancel < 2 s, no orphaned valid index |
| F | crash recovery | SIGKILL mid-build; WAL replay, no corruption |
| G | VACUUM under churn | bounded index growth at steady state |
| H | pathological patterns | NFA/compile/match/statement-timeout guards bound it |
| I | parallel saturation | no stuck-spinlock; serial==parallel results |
| J | SuRF at scale | filter size sublinear; anchored-absent reject ~O(1) |

Every row-returning check runs the **accuracy oracle** (index result set
must equal a sequential-scan result set, both directions).

## Gotchas (learned the hard way)

- **TRE progress-hook patch MUST be applied** when hand-building `libtre` for
  ANY comparison. A prior benchmark was invalidated because HEAD had the
  patch (a per-match hook that ~2× the recheck cost) while the baseline was
  pristine. `build-stack` applies `patches/tre-progress-hook.patch` before
  building libtre; keep both sides of any A/B consistent.
- **Instance-store NVMe is ephemeral** — lost on stop/terminate. That's fine
  (throwaway rig), but don't put anything you want to keep on `/mnt/nvme`.
  Pull `stress-results/` before terminating.
- **AL2023 + TRE autotools is flaky.** `build-stack` compiles `libtre.a`
  manually (bypassing the submake) and builds `lime` via the pg_tre Makefile
  rule; if `lime` is a stale/foreign binary from the tarball, delete
  `vendor/lime/lime` and let it rebuild.
- **`shared_buffers` must be < dataset** for scenario C, or you only measure
  cache hits.
- **Always `./provision-ec2.sh terminate`.** An idle i4i.8xlarge is ~$66/day.

## Interpreting results

Each run writes `~/stress-results/<host>-<UTC>/`:
- `SUMMARY.md` — human-readable per-scenario findings.
- `<scenario>.csv`, `matrix_*.csv` — raw numbers for diffing across releases.
- `meta.csv`, `estimate.txt` — corpus params, git SHA, instance, and the
  `tre_estimate_index_build` prediction vs reality.

To promote a run into the repo as a release baseline, copy its `SUMMARY.md`
to `bench/stress/RESULTS-<version>.md` (like `bench/RESULTS-v2.0-ab.md`) and
commit it. Raw `stress-results/` is gitignored.
