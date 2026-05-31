# Security Policy

## Reporting a Vulnerability

If you discover a security vulnerability in pg_tre, please
report it responsibly by emailing **greg@burd.me** with the
subject line `[pg_tre security]`.

Please include:

- A description of the vulnerability.
- Steps to reproduce.
- Potential impact.
- Any suggested fixes (optional).

I will acknowledge receipt within 7 days and aim to provide a
fix or mitigation within 30 days for confirmed vulnerabilities.

Please do not disclose the vulnerability publicly until a fix
has shipped or 90 days have passed since the initial report,
whichever comes first.

## Security Considerations

pg_tre is a PostgreSQL extension that runs as part of the
backend process and has the same privileges as the PostgreSQL
server. When deploying:

- Follow PostgreSQL security best practices.
- Limit `CREATE EXTENSION` privileges to trusted users.
- Review index configurations before deployment in
  multi-tenant environments. `pg_tre.match_timeout_ms`
  bounds the wall-clock time spent in a single regex match
  (see "Known Sharp Edges" for exactly what is and is not
  enforced). `pg_tre.max_nfa_states` rejects patterns whose
  compiled automaton is too large *before* any match runs.
  `pg_tre.compile_timeout_ms` is **advisory only** today (see
  below) — do not rely on it as a hard bound.
- Keep PostgreSQL and pg_tre updated to the latest versions.
- Run with `wal_consistency_checking = 'pg_tre'` enabled in
  pre-production environments to catch any custom-rmgr WAL
  redo bugs before they hit production standbys.

## Known Sharp Edges

- The `tre_amatch(text, text, int)` UDF accepts arbitrary
  regex patterns and runs them through a TRE backend that has
  documented worst-case complexity. Two guardrails bound this
  cost, and it is important to understand precisely what each
  one does:

  - `pg_tre.match_timeout_ms` **is enforced** for the match
    phase. The matcher's per-input-position NFA loop calls a
    progress hook (installed by the extension) that compares
    the current wall-clock time against a deadline armed when
    the match begins; once the deadline passes, the match is
    aborted and the query fails with a `query_canceled`
    error (`pg_tre: regex match exceeded
    pg_tre.match_timeout_ms`). Enforcement granularity is one
    NFA position-step (effectively sub-millisecond for normal
    inputs). The legacy `tre_amatch*` / `tre_distance` /
    `tre_similarity` UDFs arm this deadline automatically. Do
    not raise the timeout for untrusted callers.

  - `pg_tre.max_nfa_states` **is enforced** at pattern-compile
    time: a pattern whose compiled automaton exceeds the limit
    is rejected before it can ever reach the match path.

  - `pg_tre.compile_timeout_ms` is currently **advisory /
    best-effort only**. Compilation is already bounded by the
    64 KiB hard pattern-length ceiling and by
    `pg_tre.max_nfa_states`, so the dominant DoS lever is match
    time, not compile time. The compile path does not yet honor
    a wall-clock deadline; treat `compile_timeout_ms` as a
    documented intent, not a hard guarantee, and rely on
    `max_nfa_states` plus the length ceiling to bound compile
    cost.
- Tier-3 per-tuple bloom and the positional filter are
  controlled by `pg_tre.tuple_bloom_enable` (default **on**
  since 1.2.3). The chain-rank lookup defect that motivated
  disabling them in the 1.1.x line was fixed in 1.2.3 (it was
  a bloom-header reconstruction bug, not a posting-tree bug;
  see `CHANGELOG.md`). Recheck remains authoritative for
  correctness; the filters are CPU optimizations.

## Disclosure Acknowledgements

Reporters of confirmed vulnerabilities are credited in
`CHANGELOG.md` under `### Security` for the release that fixes
the issue, unless the reporter requests anonymity.
