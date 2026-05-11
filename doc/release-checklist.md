# Release Checklist: pg_tre 1.0.0

This checklist covers the steps to execute between 1.0.0-rc1 and 1.0.0 final release.

---

## Pre-Release (1.0.0-rc1)

### Code Quality

- [ ] All regression tests pass on PG18
  ```bash
  PG_CONFIG=/path/to/pg_config make clean
  PG_CONFIG=/path/to/pg_config make
  sudo PG_CONFIG=/path/to/pg_config make install
  PG_CONFIG=/path/to/pg_config scripts/run-regress.sh
  ```
  **Expected:** Tests pass: `pg_tre`, `parser`, `scan_exact`, `incremental`, `p5_read`, `planner`
  
- [ ] Zero compiler warnings (gcc + clang)
  ```bash
  # gcc
  make clean && make CC=gcc CFLAGS="-Wall -Wextra -Werror"
  
  # clang
  make clean && make CC=clang CFLAGS="-Wall -Wextra -Werror"
  ```
  
- [ ] Zero warnings from pgindent (PostgreSQL code style)
  ```bash
  pgindent --show-diff src/**/*.c include/**/*.h
  # Should produce no output
  ```
  **Note:** pgindent may not be available; visual inspection sufficient for 1.0.0.

- [ ] TAP tests execute cleanly (once Phase 5/6 bugs fixed)
  ```bash
  PG_CONFIG=/path/to/pg_config make tapcheck
  # Expected: 5 tests pass (crash_recovery, replica, reindex, upgrade, soak)
  ```
  **Status (current):** Blocked by ambuild segfault. Mark as blocker for 1.0.0 final.

### Durability

- [ ] `wal_consistency_checking='pg_tre'` runs without errors on soak workload
  ```bash
  # postgresql.conf
  wal_consistency_checking = 'pg_tre'
  
  # Run soak test
  TRE_SOAK_SEC=300 make tapcheck
  
  # Check logs for consistency errors
  grep -i "wal_consistency" /path/to/pg_log/*.log
  # Should produce no hits
  ```

- [ ] Crash recovery verified: immediate shutdown + restart preserves index consistency
  ```bash
  # In psql:
  CREATE INDEX test_idx ON large_table USING tre (body);
  -- Mid-build, in another terminal:
  pg_ctl stop -D /path/to/datadir -m immediate
  
  # Restart
  pg_ctl start -D /path/to/datadir
  
  # Verify
  REINDEX INDEX CONCURRENTLY test_idx;  -- should succeed
  SELECT COUNT(*) FROM large_table WHERE body %~~ tre_pattern('test', 1);
  # Compare to seq scan count
  ```

### Safety

- [ ] Known-bad-pattern list exercised (DoS protection)
  ```sql
  -- Catastrophic backtracking patterns
  SELECT tre_amatch('aaaaaaaaaaaaaaaaaab', '(a+)+b', 1);
  -- Expected: ERROR: regex too complex
  
  -- Deeply nested alternations
  SELECT tre_amatch('x', '(a|(b|(c|(d|e))))', 0);
  -- Expected: either succeeds or ERROR (depending on max_nfa_states)
  
  -- Large character classes
  SELECT tre_amatch('test', '[a-zA-Z0-9]{100,}', 0);
  -- Expected: compiles or ERROR (depending on max_nfa_states)
  ```
  All must either succeed cleanly or emit actionable ERROR (never hang/crash).

- [ ] Timeout enforcement verified
  ```sql
  -- Force a slow pattern (Phase 6 CHECK_FOR_INTERRUPTS)
  SET pg_tre.match_timeout_ms = 100;
  SELECT COUNT(*) FROM large_table 
  WHERE body %~~ tre_pattern('.*(a+)+b', 3);
  -- Expected: ERROR: query timeout (if pattern triggers exponential backtracking)
  ```

### Cross-Platform

- [ ] Linux build (gcc, amd64)
  ```bash
  uname -a  # Linux
  make clean && make && make install && make check
  ```

- [ ] Linux build (clang, amd64)
  ```bash
  make clean && make CC=clang && make install && make check
  ```

- [ ] macOS build (clang, arm64 and x86_64)
  ```bash
  # Apple Silicon
  uname -m  # arm64
  make clean && make && make install && make check
  
  # Intel Mac (if available)
  uname -m  # x86_64
  make clean && make && make install && make check
  ```

- [ ] PostgreSQL 18.x (latest point release)
  ```bash
  pg_config --version
  # PostgreSQL 18.3 or newer
  ```
  **Note:** pg_tre requires PG18+. Do not test on PG17.

### Documentation

- [ ] README.md reflects 1.0.0-rc1 status
  - [ ] "Status: 1.0.0-rc1 candidate" banner present
  - [ ] Feature list accurate (three-tier, k ≤ 3, native AM, WAL-logged, streaming replication)
  - [ ] Quick start example runs without error
  - [ ] Links to doc/pg_tre.md and doc/design.md valid

- [ ] CHANGELOG.md up to date
  - [ ] All Phase 0-7 work documented
  - [ ] Known limitations section honest
  - [ ] Upgrade notes from 0.1.0 present

- [ ] doc/pg_tre.md complete
  - [ ] All GUCs documented
  - [ ] All reloptions documented
  - [ ] Cookbook examples tested
  - [ ] Troubleshooting section covers known issues

- [ ] doc/migration-from-0.1.0.md tested
  - [ ] Upgrade steps execute cleanly on a test database
  - [ ] Rollback steps work (tested on a throwaway database)

### Packaging

- [ ] PGXN META.json present and valid
  ```bash
  # See doc/pgxn-meta-template.json
  cp doc/pgxn-meta-template.json META.json
  # Edit: version, abstract, license, provides
  
  # Validate
  pgxn validate META.json
  ```

- [ ] Tarball release artifact
  ```bash
  git archive --format=tar.gz --prefix=pg_tre-1.0.0/ \
    -o pg_tre-1.0.0.tar.gz v1.0.0-rc1
  
  # Test tarball
  tar xzf pg_tre-1.0.0.tar.gz
  cd pg_tre-1.0.0
  git submodule update --init  # FAILS: not a git repo
  # Document: users must clone with --recurse-submodules OR
  # ship submodules inline in release tarball
  ```
  **Decision:** Recommend git clone for 1.0.0. Tarball with vendored submodules deferred to 1.1.0.

- [ ] Homebrew formula (macOS users)
  ```ruby
  # See doc/homebrew-formula.rb
  # Test locally with `brew install --build-from-source pg_tre.rb`
  ```
  **Status:** Template only. Publish to tap after 1.0.0 final.

- [ ] Debian packaging template
  ```bash
  # See doc/debian/control
  # Test: dpkg-buildpackage -us -uc
  ```
  **Status:** Template only. Coordinate with Debian PostgreSQL maintainers post-1.0.0.

---

## Release (1.0.0-rc1 → 1.0.0)

### Tagging

- [ ] All blockers resolved (check GitHub/Codeberg issues)
  - [ ] Ambuild segfault fixed (Phase 5 bug)
  - [ ] tre_pattern_sel exported (Phase 6 bug)
  - [ ] tre_amatch signature mismatch fixed (Phase 3 bug)

- [ ] Version bump
  ```sql
  -- sql/pg_tre--1.0.0.sql: header comment unchanged (already 1.0.0)
  -- Makefile: no version field (PGXS derives from .sql)
  -- META.json: "version": "1.0.0"
  ```

- [ ] Tag commit (signed)
  ```bash
  git tag -s v1.0.0-rc1 -m "pg_tre 1.0.0 Release Candidate 1"
  git push origin v1.0.0-rc1
  ```

- [ ] After RC testing period (1-2 weeks), tag final
  ```bash
  git tag -s v1.0.0 -m "pg_tre 1.0.0 — Native approximate-regex index AM"
  git push origin v1.0.0
  ```

### Announcement

- [ ] Draft announcement (see doc/announcement.md)
  - [ ] What pg_tre does (1 paragraph)
  - [ ] Who should use it (2 sentences)
  - [ ] How to try it (3 commands)
  - [ ] Link to documentation

- [ ] Post to pgsql-announce mailing list
  ```
  To: pgsql-announce@postgresql.org
  Subject: pg_tre 1.0.0 — Approximate regex index for PostgreSQL 18+
  [Body: see doc/announcement.md]
  ```

- [ ] Post to relevant communities
  - [ ] Hacker News (Show HN)
  - [ ] Reddit /r/PostgreSQL
  - [ ] Planet PostgreSQL (blog post if you have one)
  - [ ] PostgreSQL Wiki: Extensions page

### Distribution

- [ ] Upload to PGXN
  ```bash
  pgxn-manager release pg_tre-1.0.0.tar.gz
  ```
  **Note:** Requires PGXN account. Create at https://manager.pgxn.org/

- [ ] Create GitHub/Codeberg release
  - [ ] Tag: v1.0.0
  - [ ] Title: pg_tre 1.0.0 — Native Approximate-Regex Index AM
  - [ ] Body: copy from CHANGELOG.md "1.0.0" section
  - [ ] Attach: tarball (if applicable)

- [ ] Update project website (if exists)
  - [ ] Latest version: 1.0.0
  - [ ] Download link: Codeberg releases page
  - [ ] Documentation link: doc/pg_tre.md

---

## Post-Release

### Monitoring

- [ ] Watch issue tracker for 1.0.0 bugs (first 2 weeks critical)
- [ ] Triage incoming reports by severity:
  - **Critical:** Data loss, crash, security → patch immediately
  - **High:** Wrong results, performance regression → 1.0.1 within 1 week
  - **Medium:** Usability, missing feature → 1.1.0
  - **Low:** Documentation, cosmetic → 1.x.x or won't-fix

### Documentation Feedback

- [ ] Update docs based on user questions (first month)
  - Common stumbling blocks → add to Troubleshooting
  - Unclear instructions → revise for clarity
  - Missing examples → add to Cookbook

### Versioning Policy

Establish and document:
- **Patch releases (1.0.x):** Bug fixes, documentation updates, no new features, no on-disk format changes
- **Minor releases (1.x.0):** New features, performance improvements, no backward-incompatible changes
- **Major releases (x.0.0):** Breaking changes, on-disk format changes (require dump/restore)

---

## Rollback Plan

If a critical bug is found post-release:

1. **Immediate:** Post advisory to pgsql-announce + project README
   - Affected versions
   - Symptom description
   - Workaround (if any)
   - Fix ETA

2. **Short-term (< 48 hours):** Release 1.0.1 with fix
   ```bash
   git checkout v1.0.0
   git cherry-pick <fix-commit>
   # Test thoroughly
   git tag -s v1.0.1 -m "pg_tre 1.0.1 — Critical bugfix release"
   git push origin v1.0.1
   ```

3. **Long-term:** Post-mortem
   - Root cause analysis
   - Add regression test for bug
   - Update release checklist to catch similar issues

---

## Checklist Summary

**Pre-release (must all pass):**
- [ ] Regression tests green
- [ ] Zero warnings (gcc + clang)
- [ ] TAP tests green (once bugs fixed)
- [ ] wal_consistency_checking clean
- [ ] DoS patterns rejected cleanly
- [ ] Builds on Linux + macOS, PG18+
- [ ] Documentation complete and accurate

**Release:**
- [ ] Blockers resolved
- [ ] Tag v1.0.0
- [ ] Announcement posted
- [ ] PGXN upload

**Post-release:**
- [ ] Issue monitoring (2 weeks intensive)
- [ ] Documentation updates based on feedback

---

## Sign-Off

Before tagging 1.0.0, verify:

```bash
# 1. Clean build
make clean && make && make install

# 2. All tests pass
make check  # regression
make tapcheck  # durability (when unblocked)

# 3. No uncommitted changes
git status  # should be clean

# 4. Documentation links valid
grep -r '\[.*\](.*\.md)' doc/*.md README.md | \
  while read line; do
    file=$(echo "$line" | sed -n 's/.*(\(.*\.md\)).*/\1/p')
    [ -f "$file" ] || echo "BROKEN: $line"
  done
# Should produce no output

# 5. Version strings consistent
grep -r '1\.0\.0' sql/*.sql META.json CHANGELOG.md
# All should reference 1.0.0 (not 1.0.0-dev or 1.0.0-rc1)
```

If all checks pass:
```bash
git tag -s v1.0.0 -m "pg_tre 1.0.0 — Native approximate-regex index AM for PostgreSQL 18+"
git push origin v1.0.0
```

**Congratulations!** 🎉
