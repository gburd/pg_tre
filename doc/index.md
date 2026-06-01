# pg_tre

**A native PostgreSQL 18+ index access method for fast approximate
(fuzzy) regular-expression matching over text columns.**

pg_tre turns the classic "find text that looks like this, maybe with a
typo" problem into a SQL-composable indexed query. A three-tier filter
funnel (range bloom → trigram posting trees → per-tuple bloom) narrows
the candidate set before any heap recheck, with the
[TRE library](https://github.com/laurikari/tre) performing the exact
edit-distance match.

```sql
SELECT id FROM docs
WHERE body %~~ tre_pattern('(error){~1}.*(42[0-9]){~0}', 1);
-- Bitmap Index Scan on docs_tre  →  sub-millisecond on 10k rows.
```

## Where to start

- **[User Guide](pg_tre.md)** — installation, operators, query syntax,
  tuning, and worked examples. Start here.
- **[Design](design.md)** — architecture of the three-tier funnel and
  the on-disk structures.
- **[On-disk page format](onpage_format.md)** — byte-level page layout
  reference.
- **[Performance](perf.md)** — measured numbers and methodology.
- **[Testing](testing.md)** — the regression / sanitizer / stress
  apparatus.

## Project links

- Source & issues: <https://codeberg.org/gregburd/pg_tre>
- GitHub mirror: <https://github.com/gburd/pg_tre>
- License: MIT (bundles TRE under BSD-2-Clause)
