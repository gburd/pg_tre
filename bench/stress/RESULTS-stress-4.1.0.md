# Stress results — pg_tre 4.1.0

Vendored sparsemap **v5.5.0 → v5.6.0** (upstream security hardening) plus the
corruption detection that release makes necessary.

## Rig

Different from the usual rig, which is itself useful coverage: previous
releases were qualified on Amazon Linux 2023 / gcc 11, this one on **Debian
12.15 / gcc 12.2.0**. Same code, different libc and compiler.

| | |
|---|---|
| Instance | `c7i.4xlarge` (16 vCPU, 30 GB), 80 GB gp3, us-east-2 |
| OS / compiler | Debian 12.15, gcc 12.2.0 |
| PostgreSQL | 18.0, `--enable-tap-tests`, `data_checksums=on` |
| pg_tre | 4.1.0 (TRE 0.9.0), sparsemap 5.6.0 |
| Corpus | 1,000,000 rows, shape=medium |
| Storage | EBS gp3 (no instance NVMe on this type) |

## Gates

| Gate | Result |
|---|---|
| Build | 0 warnings, 0 errors |
| Regression | **45/45** |
| TAP | **17/17** (3 files, 5,379 s) |
| Accuracy oracle | **0 mismatches** (16 query panels) |
| Stuck spinlocks | 0 |

## Stress scenarios

| Scenario | Result |
|---|---|
| C — cold-cache (working set >> 4 GB shared_buffers) | cold + warm matrices, oracle clean |
| D — CREATE INDEX CONCURRENTLY under writes | `valid=t`, 0 mismatches, hits 59,167 |
| G — VACUUM under churn, row count flat | **PASS**, 2146 MB = **1.98x** baseline (bounded <2x) |
| H — pathological patterns vs DoS guards | compile_bomb rejected in 0.01 s; big_k 8.05 s → 286,702 |
| I — parallel-build saturation + determinism | parallel 485.3 s / serial 628.3 s, **same_hits=YES** (59,167) |
| J — SuRF at scale | anchored-absent reject **p50 0.070 ms** (O(1) in rows) |

Scenario G's 1.98x is the known, documented steady state — it plateaus by
round 3 (2114 → 2136 → 2138 → 2141 → 2146 MB) rather than growing.

## sparsemap v5.6.0 qualification

The library swap was qualified separately from pg_tre, because a verbatim
vendor bump is only as safe as the evidence that the new bytes behave like the
old ones.

| Check | Result |
|---|---|
| Consumed API signatures (17 `sm_*`) | unchanged from v5.5.0 |
| **Cross-version read** — v5.5.0 writes, v5.6.0 reads | **10/10 shapes**, identical cardinality, 0 failures |
| pg_tre's own map shapes vs stricter validator | 32/32 pass (new rejection cannot fire on our output) |
| Upstream suite vs **vendored** copy | 44/44 API/scale; 175,541 coverage expectations |
| Upstream hardening suites | NULL contract 69 fns; S1 validate; S3 split safety; S4 amplification (46 pairs, 0 mismatches); heisenbug 528/528 |
| `SPARSEMAP_PREFIX=__tre_` renaming | 85 prefixed symbols, **0 unprefixed leaks** |

Cross-version is the gate that matters for the upgrade: every posting page
already on disk was written by v5.5.0.

## Corruption detection

v5.6.0's `sm_open` validates, and since it returns void it cannot report
failure — it substitutes an **empty map** for bytes it rejects. Unchecked
that is a silent wrong answer, and a VACUUM of that leaf would make the loss
permanent.

Predicate selection was **measured, not reasoned**:

| Predicate | Corruptions detected | False positives |
|---|---|---|
| `!sm_validate(map)` (first attempt) | **0 of 4** | — |
| `sm_get_size(map) != n` (shipped) | **4 of 4** | **0 of 9 shapes** |

`sm_validate` fails because the substituted empty map is itself structurally
valid. Writing the test before trusting the reasoning is what caught it.

Corruptions covered: inflated chunk count, misaligned chunk start, RLE length
> capacity, truncated mid-chunk.

### End-to-end, in the server

Corrupting the inline blob of a live `upper_leaf` page:

```
ERROR:  pg_tre: corrupt inline sparsemap in posting for this trigram
HINT:  REINDEX the index to rebuild it.
```

**Scope, stated honestly:** with `data_checksums=on` (the PG18 default),
PostgreSQL catches on-disk damage *first* — `invalid page in block N`. The
guards only became reachable after `pg_checksums --disable`. They are
defence-in-depth for checksums-off clusters and for damage arising in memory
after the checksum was verified — not the primary defence, and this report
would rather say so than overstate them.

## Notes

- All postings in the corpus lived **inline in upper leaves**; the index had
  zero `posting_leaf` pages. That is why the inline path is the guard site
  that matters most, and it is worth remembering when reasoning about which
  read path a workload actually exercises.
- EBS-only instance type, so `/mnt/nvme` is a plain directory on gp3 rather
  than instance storage; absolute latencies are not comparable to NVMe runs,
  but the A/B and determinism gates are unaffected.
