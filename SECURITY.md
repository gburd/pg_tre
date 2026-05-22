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
  multi-tenant environments — index options like
  `pg_tre.compile_timeout_ms` and `pg_tre.match_timeout_ms`
  exist specifically to bound the cost of malicious or
  pathological regex inputs.
- Keep PostgreSQL and pg_tre updated to the latest versions.
- Run with `wal_consistency_checking = 'pg_tre'` enabled in
  pre-production environments to catch any custom-rmgr WAL
  redo bugs before they hit production standbys.

## Known Sharp Edges

- The `tre_amatch(text, text, int)` UDF accepts arbitrary
  regex patterns and runs them through a TRE backend that has
  documented worst-case complexity. The
  `pg_tre.match_timeout_ms` GUC bounds wall-clock time per
  match; do not raise it for untrusted callers.
- Tier-3 per-tuple bloom and the positional filter are
  bypassed when `pg_tre.tuple_bloom_enable = off` (default
  off in 1.1.x). Re-enabling them in environments that build
  multi-leaf posting trees may change result counts until the
  chain-rank lookup is repaired (see `STATUS.md` v1.2
  followups). Recheck remains authoritative for correctness;
  the filters are CPU optimizations.

## Disclosure Acknowledgements

Reporters of confirmed vulnerabilities are credited in
`CHANGELOG.md` under `### Security` for the release that fixes
the issue, unless the reporter requests anonymity.
