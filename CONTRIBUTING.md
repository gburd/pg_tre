# Contributing to pg_tre

Thank you for your interest in contributing to pg_tre. This
document covers what you need to know to land a patch.

## Getting Started

### Development Setup

```sh
git clone --recurse-submodules https://codeberg.org/gregburd/pg_tre
cd pg_tre
```

Install PostgreSQL 18 with development headers. The project
develops against the pgrx-managed PG18 install at
`~/.pgrx/18.3/pgrx-install/`, but any working PG18 install with
`pg_config` on `$PATH` works.

```sh
PG_CONFIG=$HOME/.pgrx/18.3/pgrx-install/bin/pg_config make
PG_CONFIG=$HOME/.pgrx/18.3/pgrx-install/bin/pg_config make install
```

Run the tests:

```sh
PGHOST=$HOME/.pgrx PGPORT=28818 \
    PG_CONFIG=$HOME/.pgrx/18.3/pgrx-install/bin/pg_config \
    bash scripts/run-regress.sh
```

### Pre-commit Hooks (Recommended)

```sh
pip install pre-commit
pre-commit install
```

This installs the clang-format and trailing-whitespace hooks
defined in `.pre-commit-config.yaml`. The same checks run in
the `formatting.yml` CI workflow on every PR; running them
locally avoids the round-trip.

## Code Style

We follow PostgreSQL coding conventions, enforced by
`clang-format` configured in `.clang-format`. Key points:

- **Line limit**: 79 characters.
- **Indentation**: tabs, 4-space tab width.
- **Brace style**: Allman (opening braces on new lines).
- **Naming**: snake_case for functions and variables.
- **Comments**: 2 spaces before trailing comments.
- **Headers**: include guards (`#ifndef ... #define ... #endif`)
  with the file's basename uppercased.
- **Includes**: `postgres.h` first, then standard library
  headers (`<...>`), then project headers (`"pg_tre/..."`).

Format your code before committing:

```sh
make format        # auto-format src/ and include/
make format-check  # check formatting without changes
make format-diff   # show what `make format` would change
```

### Source Code Architecture

The `src/` directory is organized into layers. Upper layers
depend on lower layers, not the reverse. The directory structure
communicates the dependency flow.

**Layer 1 — Postgres interface:**
- `src/am/` — Index access method (handler, build, scan,
  vacuum, cost, options, selectivity).
- `src/module.c` — Extension init, GUC registration, custom
  rmgr registration, legacy UDFs.

**Layer 2 — Index coordination:**
- `src/pages/` — Page-level read/write helpers (meta, upper,
  posting, pending, range, buffer).
- `src/wal/` — WAL record encode/decode + redo for the custom
  resource manager.

**Layer 3 — Query processing:**
- `src/query/` — Regex AST, parser, tokens, trigram
  extraction, tile expansion, DNF/CNF resolution, debug helpers.

**Layer 4 — Storage primitives:**
- `src/util/` — Vendored sparsemap, bloom, hash, UTF-8,
  pattern cache, type pattern, TRE match wrapper.

**Cross-cutting:**
- `src/util/sparsemap.c` is vendored from
  https://codeberg.org/gregburd/sparsemap with adjusted
  include paths. Do not edit it directly; instead, file an
  upstream issue and re-vendor on the next sparsemap release.

**Dependency rules:**
- Layer 1 may depend on Layer 2, 3, 4.
- Layer 2 may depend on Layer 3, 4.
- Layer 3 may depend on Layer 4.
- Layer 4 should not depend on Layer 1, 2, or 3.

These rules are enforced by convention and code review, not
mechanically.

### Include Path Convention

All project-local `#include` directives use full paths relative
to `include/`:

```c
#include "pg_tre/posting.h"   /* correct */
#include "pg_tre/sparsemap.h" /* correct */
#include "posting.h"          /* wrong: missing prefix */
#include "../posting.h"       /* wrong: relative path */
```

Postgres system includes (`<postgres.h>`, `<access/genam.h>`,
etc.) are unaffected.

### File Size

Split files based on responsibility, not line count. A
1500-line file with a single clear purpose is fine. A 400-line
file doing three unrelated things should be split.

## Testing Requirements

Before submitting a pull request:

1. **Build** — `make` must succeed without warnings:
   ```sh
   PG_CONFIG=... make 2>&1 | grep -E "warning:|error:" \
       && echo FAIL || echo OK
   ```
2. **Regression** — full suite must pass three runs in a row
   (catches flakes):
   ```sh
   for i in 1 2 3; do
       PG_CONFIG=... bash scripts/run-regress.sh \
           | grep -E "^FAIL" && echo "FLAKE on run $i"
   done
   ```
3. **Format** — `make format-check` must pass.
4. **Upgrade compatibility** — if you touched
   `pg_tre.control`, any `sql/pg_tre--*.sql`, the metapage
   layout, the on-disk format constants, or any WAL record
   format, the `upgrade-tests.yml` workflow will fail in CI
   unless your PR includes a matching upgrade-script update.

If you modify error messages or NOTICE text, update the
corresponding expected output files in `test/expected/`.

### Adding a Regression Test

Tests live in `test/sql/<name>.sql` with paired expected output
in `test/expected/<name>.out`. Register the test in
`test/expected/` by running the suite once and snapshotting:

```sh
bash scripts/run-regress.sh <new-test-name>
cp test/results/<new-test-name>.out test/expected/<new-test-name>.out
```

Then add `<new-test-name>` to the test list in
`scripts/run-regress.sh` so the full suite picks it up.

### Differential Tests

When the index is supposed to return the same answer as a
seq-scan, write the test as a differential check. The
`test/sql/multi_leaf.sql` test ends with this pattern:

```sql
SET enable_seqscan = off;
CREATE TEMP TABLE idx_ids AS SELECT id FROM t WHERE ...;

SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE seq_ids AS SELECT id FROM t WHERE ...;

SELECT
    (SELECT count(*) FROM idx_ids) = (SELECT count(*) FROM seq_ids)
        AS counts_agree,
    NOT EXISTS (SELECT 1 FROM idx_ids EXCEPT SELECT 1 FROM seq_ids)
    AND NOT EXISTS (SELECT 1 FROM seq_ids EXCEPT SELECT 1 FROM idx_ids)
        AS row_sets_agree;
```

A frozen-broken expected output cannot hide a regression that
makes the index drop rows; this assertion will print `f` for
either column and fail the test.

## Commit Guidelines

- Imperative mood, ≤72 character subject line.
- One logical change per commit.
- Reference issues with `#NNN` when applicable.
- Body explains the *why*, not the *what* — the diff already
  shows what changed.
- Never amend or rebase commits already pushed to shared
  branches.
- Never force-push.

## Pull Request Process

1. Fork the repository (or push a branch if you have access).
2. Create a feature branch off `main`.
3. Make your changes with appropriate tests.
4. Ensure all tests pass locally.
5. Submit a pull request to `main`.

All pull requests are tested against PostgreSQL 18 in CI. The
`upgrade-tests` workflow runs against every shipped version in
the matrix when SQL or the control file changed.

### PR Description

Include:

- A brief summary of the change.
- The problem it solves (or feature it adds).
- Testing notes — what you ran, what you saw.
- Any breaking changes or migration notes.
- Linked issues.

## Reporting Issues

Use the issue templates under `.github/ISSUE_TEMPLATE/`:

- **Bug Report** — include PG version, pg_tre version,
  operating system, steps to reproduce, expected vs. actual.
- **Feature Request** — describe the problem you're trying to
  solve, the proposed solution, and any alternatives
  considered.

## License

By contributing to pg_tre, you agree that your contributions
will be licensed under the MIT License (see `LICENSE`).
