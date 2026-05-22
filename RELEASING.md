# Releasing pg_tre

This document describes the release process for pg_tre.

## Version Scheme

Semantic versioning: `MAJOR.MINOR.PATCH`.

- Release versions are plain triples (e.g. `1.1.0`, `1.1.1`, `1.2.0`).
- In-flight development between releases uses the `-dev` suffix on
  the next planned version (e.g. `1.2.0-dev`).
- Patch releases ship bug fixes and library-vendor refreshes that
  don't change the on-disk format or the SQL surface.
- Minor releases may add SQL surface or new GUCs but keep the
  on-disk format compatible (no REINDEX required).
- Major releases may break the on-disk format. Document the
  required user action in the release notes.

## Cutting a Release

### 1. Audit the upgrade SQL script

This is the one piece of the release that cannot be automated, and
the piece most likely to be wrong.

The upgrade script `sql/pg_tre--PREV--CURRENT.sql` must recreate,
for an installation still on `PREV`, every catalog object that
exists in the current main install file but not in the previous
release's main install file. To enumerate what the upgrade script
must cover, diff the two main install files:

```sh
git show vPREV:sql/pg_tre--PREV.sql > /tmp/prev.sql
diff /tmp/prev.sql sql/pg_tre--CURRENT.sql
```

For every statement that is new in the current main file, verify
the upgrade script has a matching statement:

| New in main file              | Required in upgrade script        |
|-------------------------------|-----------------------------------|
| `CREATE FUNCTION`             | `CREATE FUNCTION`                 |
| `CREATE OPERATOR`             | `CREATE OPERATOR`                 |
| `CREATE OPERATOR CLASS`       | `CREATE OPERATOR CLASS`           |
| `CREATE TYPE`                 | `CREATE TYPE`                     |
| `CREATE CAST`                 | `CREATE CAST`                     |
| `ALTER OPERATOR FAMILY`       | `ALTER OPERATOR FAMILY`           |
| Catalog `UPDATE pg_catalog.*` | Same `UPDATE`                     |

Renamed, signature-changed, or removed objects need matching
`DROP` / `ALTER` statements in the upgrade script.

The `upgrade-tests` CI workflow exercises the upgrade against
every version in its `old_version` matrix and is the primary
safety net for gaps — but it can only catch what the regression
suite touches. A new operator that has no test coverage will pass
CI and ship broken. The audit catches what the workflow can't.

### 2. Run the bump script

```sh
./scripts/bump-version.sh CURRENT-dev CURRENT
```

The script auto-detects mode from the version-string syntax:

- `OLD=A.B.C-dev`, `NEW=A.B.C` (same triple) → release bump.
- `OLD=A.B.C`, `NEW=X.Y.Z-dev` → next-cycle dev bump.

It renames the SQL files, updates every version reference across
`pg_tre.control`, `Makefile`, `META.json`,
`include/pg_tre/pg_tre.h`, `STATUS.md`, `CHANGELOG.md`,
`debian/changelog`, and `packaging/pg_tre.spec`, and emits a
"stragglers" warning for any literal version strings it didn't
expect to find. Read the warning before committing.

### 3. Update `STATUS.md` and `CHANGELOG.md`

The bump script updates the version header. The release notes
themselves are content; you write them.

`CHANGELOG.md` follows the [Keep a Changelog] format.
Group entries under `### Added`, `### Changed`, `### Fixed`,
`### Removed`, `### Security`. Always include:

- A one-paragraph summary at the top of the section.
- The on-disk-format compatibility statement (no re-index
  required, REINDEX required, etc.).
- Acknowledgements for upstream contributions you depended on.

[Keep a Changelog]: https://keepachangelog.com/en/1.0.0/

### 4. Add the previous release to the upgrade-tests matrix

In `.github/workflows/upgrade-tests.yml`, add `PREV` to the
`old_version` matrix so future releases are tested for upgrade
compatibility from this version. The matrix grows by one entry
per release; do not skip this step.

### 5. Run the release-check script locally

```sh
PG_CONFIG=$HOME/.pgrx/18.3/pgrx-install/bin/pg_config \
    bash scripts/release-check.sh
```

Verifies a clean build with zero warnings, the regression suite
passes, the bench smoke completes, and no committed binaries
landed.

### 6. Open the PR and tag

```sh
git checkout -b release-CURRENT
git add -A
git commit -m "Release pg_tre CURRENT"
git push origin release-CURRENT
# Open PR, get review, merge.

# After merge:
git checkout main && git pull
git tag -a vCURRENT -m "pg_tre CURRENT"
git push origin vCURRENT
```

Codeberg picks up the tag and creates a release entry with the
auto-generated tarball.

## Bumping to the Next Dev Version

After a release is published, open a follow-up PR:

```sh
git checkout main && git pull
git checkout -b bump-to-NEXT-dev
./scripts/bump-version.sh CURRENT NEXT-dev
git add -A
git commit -m "chore: bump version to NEXT-dev"
```

The script creates the new main SQL file (renamed from the
previous release's), creates a stub upgrade file
`sql/pg_tre--CURRENT--NEXT-dev.sql` containing only a header
comment, and updates every other version reference. Contributors
append catalog DDL to the upgrade file as features land in the
dev cycle.

## SQL Upgrade Path Requirements

**Upgrade scripts must form a single linear chain.** Every
version connects to exactly one next version — no shortcuts that
skip intermediate steps. This keeps the number of upgrade scripts
minimal and the path predictable.

```
0.1.0 → 1.0.0 → 1.1.0 → 1.1.1 → 1.2.0 → ...
```

Every release must have an upgrade path from the previous stable
release (e.g. `1.1.0--1.1.1.sql`). Dev versions are not supported
for direct upgrades; users on dev versions reinstall.

## Upgrade Compatibility Matrix

Not all version transitions are compatible with `ALTER EXTENSION
UPDATE`. Breaking changes that require index recreation or server
restart:

| Change Type | Impact | User Action |
|-------------|--------|---------------------|
| On-disk format version bump | Existing indexes incompatible | `REINDEX` |
| WAL record layout change | Standby recovery breaks on mixed versions | Restart standbys after upgrade, verify wal_consistency_checking |
| GUC default change | Behavior change after restart | Document in CHANGELOG |
| Vendored library API change | No user-visible effect | None |

**Current compatibility matrix:**

| From    | To      | Compatible?  | Notes                         |
|---------|---------|--------------|-------------------------------|
| 0.1.0   | 1.0.0   | ❌ No        | UDF-only → native AM; recreate |
| 1.0.0   | 1.1.0   | ✅ Yes       | Same on-disk format           |
| 1.1.0   | 1.1.1   | ✅ Yes       | sparsemap hardening only      |

When releasing a version with breaking changes:

1. Update `.github/workflows/upgrade-tests.yml` to exclude
   incompatible versions from the matrix (set the `exclude:`
   list).
2. Document the required action in `CHANGELOG.md` under
   `### Breaking changes`.
3. Provide a migration guide for major version bumps in
   `doc/migration-from-X.Y.Z.md`.

## On-Disk Format Versions

If any on-disk format changed during a release cycle, bump the
corresponding version constant before tagging:

| Constant                 | Header                           | Purpose                       |
|--------------------------|----------------------------------|-------------------------------|
| `PG_TRE_FORMAT_VERSION`  | `include/pg_tre/page.h`          | Index meta + page format      |
| `PG_TRE_RMGR_VERSION`    | `include/pg_tre/xlog.h`          | Custom rmgr WAL record format |

Version bumps break upgrade compatibility — exclude incompatible
old versions from the upgrade-tests matrix and document the
breaking change in release notes.

## Automated Workflows

| Workflow              | Trigger                                  | Purpose                              |
|-----------------------|------------------------------------------|--------------------------------------|
| `ci.yml`              | PR, push to main                         | Build + test on PG18 (gcc, clang)    |
| `formatting.yml`      | PR (src/**)                              | clang-format check                   |
| `pgspot.yml`          | push to main, PR (sql/**)                | Extension SQL security scan          |
| `upgrade-tests.yml`   | PR (sql/** or pg_tre.control), weekly    | ALTER EXTENSION UPDATE matrix        |

## Troubleshooting

### Old SQL files in the Postgres share directory

If tests fail with old version messages, check for stale files:

```sh
ls $(pg_config --sharedir)/extension/pg_tre*
```

Remove old dev versions that shouldn't be installed:

```sh
rm $(pg_config --sharedir)/extension/pg_tre--X.Y.Z-dev.sql
```

### Extension won't upgrade

If `ALTER EXTENSION pg_tre UPDATE` fails, verify:

1. The upgrade SQL file exists in the share directory.
2. `pg_tre.control` has the correct `default_version`.
3. The upgrade path exists:
   ```sql
   SELECT * FROM pg_extension_update_paths('pg_tre');
   ```
4. The cluster has been restarted with the new `pg_tre.so`
   loaded via `shared_preload_libraries`. The ALTER runs against
   the binary already in memory; replacing the file alone does
   not refresh the postmaster's mapped copy.

### Cluster won't start after `make install`

`shared_preload_libraries = 'pg_tre'` requires the new `.so` to
be ABI-compatible with the postmaster's existing state. Stop the
postmaster fully (`pg_ctl stop`), then start it (`pg_ctl start`).
A `pg_ctl restart` may not free the old shared-memory segment;
if `pg_ctl start` fails with "pre-existing shared memory block
... is still in use", `ipcrm -m <id>` the stale segment from the
log message and retry.
