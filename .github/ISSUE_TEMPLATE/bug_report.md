---
name: Bug Report
about: Report a bug in pg_tre
title: ''
labels: bug
assignees: ''
---

## Bug Description

A clear and concise description of the bug.

## Environment

- **PostgreSQL version**: (e.g. 18.3)
- **pg_tre version**: (`SELECT tre_version();`)
- **Operating system**: (e.g. Ubuntu 24.04, macOS 14, NixOS 24.05)
- **Build flavor**: (from source, distro package, etc.)
- **`shared_preload_libraries`**: (the line from `postgresql.conf`)

## Steps to Reproduce

1. Create table…
2. Build index…
3. Run query…
4. See error / wrong result.

```sql
-- Minimal SQL that reproduces the issue
CREATE TABLE t (id serial PRIMARY KEY, body text);
INSERT INTO t (body) SELECT '...' FROM generate_series(1, ...);
CREATE INDEX t_idx ON t USING tre (body);
-- The query that misbehaves:
EXPLAIN ANALYZE SELECT ... FROM t WHERE body %~~ tre_pattern('...', 0);
```

## Expected Behavior

What you expected to happen.

## Actual Behavior

What actually happened. Include error messages, NOTICE
output, and the result row counts (or the wrong rows
returned).

## Differential Check

If the issue is "the index returns different rows than a
seq-scan," please include the seq-scan result so the
discrepancy is unambiguous:

```sql
SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM t WHERE body %~~ tre_pattern('...', 0);
-- vs
SET enable_seqscan = off;
SELECT count(*) FROM t WHERE body %~~ tre_pattern('...', 0);
```

## Server Logs

Any relevant lines from the PostgreSQL server log,
particularly anything tagged `pg_tre:` or `WARNING`.

## Additional Context

Any other context — non-default GUC values, table size,
data distribution, recent recovery / failover events, etc.
