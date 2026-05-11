# Migration Guide: pg_tre 0.1.0 → 1.0.0

This guide covers upgrading from the UDF-only 0.1.0 release to the 1.0.0 native index access method.

---

## Overview

**0.1.0:** UDF-only extension. Provided `tre_amatch*` functions that ran TRE's regex engine directly on every row (seq scan only).

**1.0.0:** Native index access method. Adds the `tre` AM, `tre_pattern` type, `%~~` operator, and indexing support. Legacy UDFs preserved for backward compatibility.

**Key change:** `shared_preload_libraries = 'pg_tre'` now required for index AM functionality (rmgr registration).

---

## Prerequisites

- PostgreSQL 18 or newer
- Existing database with pg_tre 0.1.0 installed
- Superuser access for `shared_preload_libraries` modification

---

## Upgrade Steps

### 1. Build and Install 1.0.0

```bash
cd /path/to/pg_tre
git pull origin main  # or download 1.0.0 release tarball
git submodule update --init --recursive

PG_CONFIG=/path/to/pg_config make clean
PG_CONFIG=/path/to/pg_config make
sudo PG_CONFIG=/path/to/pg_config make install
```

Verify installation:
```bash
ls -l $(pg_config --pkglibdir)/pg_tre.so
# Should show recent timestamp
```

### 2. Enable Preload (Required for Index AM)

Edit `postgresql.conf`:
```ini
shared_preload_libraries = 'pg_tre'
```

**If you have other preloaded libraries:**
```ini
shared_preload_libraries = 'pg_stat_statements,pg_tre'
```

Restart PostgreSQL:
```bash
pg_ctl restart -D /path/to/datadir
# OR
systemctl restart postgresql
```

**Without preload:**
- Legacy UDFs (`tre_amatch*`) continue to work
- `CREATE INDEX USING tre` will fail with: `ERROR: custom rmgr not registered`

### 3. Run the Upgrade Script

Connect to each database using pg_tre:
```sql
\c your_database
ALTER EXTENSION pg_tre UPDATE TO '1.0.0';
```

What this does:
- Registers the `tre` access method handler
- Creates the `tre_text_ops` operator class
- Does NOT drop or modify legacy UDFs (backward compatible)

Verify:
```sql
SELECT extname, extversion FROM pg_extension WHERE extname = 'pg_tre';
--  extname | extversion 
-- ---------+------------
--  pg_tre  | 1.0.0

\dAm tre
-- Access method: tre
--   Handler: tre_handler
```

### 4. Verify Legacy UDFs Still Work

```sql
SELECT tre_amatch('hello', 'helo', 1);
-- Returns: t (backward compatible)

SELECT tre_version();
-- Returns: TRE 0.9.0 (BSD)
```

**No changes required to existing application queries using legacy UDFs.**

### 5. Optionally Create Indexes

```sql
CREATE INDEX docs_body_tre_idx ON documents USING tre (body);
```

**No automatic migration:** 0.1.0 had no indexes. If you want index-accelerated queries, create them manually.

**Rewrite queries to use `%~~` for indexing:**

Before (always seq scan):
```sql
SELECT * FROM documents WHERE tre_amatch(body, 'environment', 2);
```

After (uses index if present):
```sql
SELECT * FROM documents WHERE body %~~ tre_pattern('environment', 2);
```

Both syntaxes work; only `%~~` is indexable.

---

## What Changes

### Added

- **Access method:** `CREATE INDEX ... USING tre` now supported
- **Type:** `tre_pattern` (compiled regex with edit-distance budget)
- **Operator:** `%~~` (text, tre_pattern) → bool (indexable)
- **Functions:**
  - `tre_pattern(text, ...)` constructors
  - `tre_parse_debug(text)` — show parsed AST
  - `tre_extract_debug(text)` — show extracted trigrams
- **GUCs:**
  - `pg_tre.default_max_cost`
  - `pg_tre.max_nfa_states`
  - `pg_tre.compile_timeout_ms`
  - `pg_tre.match_timeout_ms`
  - `pg_tre.max_extraction_fanout`
  - `pg_tre.pending_list_limit`
  - `pg_tre.range_size_blocks`
  - `pg_tre.bloom_tuple_bits`
  - `pg_tre.fastupdate`
  - `pg_tre.tuple_bloom_enable`
- **Reloptions:** `pending_list_limit`, `bloom_tuple_bits`, `range_size_blocks`, `fastupdate`, `tuple_bloom_enable`, `q`

### Unchanged

- All legacy UDFs: `tre_amatch`, `tre_amatch_cost`, `tre_amatch_detail`, `tre_version`
- Function signatures identical
- Return types identical
- Behavior identical (modulo GUC-controlled safety limits)

### Removed

**Nothing.** 1.0.0 is 100% backward compatible with 0.1.0 UDF usage.

---

## Behavior Changes

### NOTICE Output

**0.1.0:**
```
NOTICE: TRE approximate match: cost 2, operations: 1 ins, 0 del, 1 subst
```

**1.0.0:**
```
NOTICE: pg_tre: index build complete, 1234 tuples, 567 distinct trigrams
```

NOTICE messages changed during index operations. If you parse NOTICE output, update your scripts.

### Safety Limits

**New in 1.0.0:** DoS protection GUCs reject pathological patterns:

```sql
SELECT tre_amatch('input', '(a+)+b', 3);
-- 0.1.0: hangs (catastrophic backtracking)
-- 1.0.0: ERROR: regex too complex (estimated NFA states exceed pg_tre.max_nfa_states)
```

To allow complex patterns:
```sql
SET pg_tre.max_nfa_states = 50000;
SET pg_tre.compile_timeout_ms = 5000;
```

### Performance

**Without indexes:** Identical to 0.1.0 (seq scan + TRE regaexec).

**With indexes:** 10-1000x faster for selective patterns (k ≤ 2, long literal runs).

---

## Rollback

If you need to downgrade to 0.1.0:

### 1. Drop All pg_tre Indexes

```sql
DROP INDEX docs_body_tre_idx;
-- Repeat for all USING tre indexes
```

Verify:
```sql
SELECT indexrelid::regclass 
FROM pg_index i 
JOIN pg_class c ON i.indexrelid = c.oid 
JOIN pg_am a ON c.relam = a.oid 
WHERE a.amname = 'tre';
-- Should return 0 rows
```

### 2. Downgrade Extension

```sql
ALTER EXTENSION pg_tre UPDATE TO '0.1.0';
```

**Note:** The 0.1.0 → 1.0.0 upgrade script is NOT reversible. If this fails, you must:
```sql
DROP EXTENSION pg_tre CASCADE;
-- Reinstall 0.1.0 binaries, then:
CREATE EXTENSION pg_tre VERSION '0.1.0';
```

### 3. Remove Preload

Edit `postgresql.conf`:
```ini
# shared_preload_libraries = 'pg_tre'  # comment out or remove
```

Restart PostgreSQL.

### 4. Verify

```sql
SELECT tre_amatch('test', 'test', 0);
-- Should work (legacy UDFs)

CREATE INDEX test_idx ON test USING tre (col);
-- Should fail: ERROR: access method "tre" does not exist
```

---

## Troubleshooting

### Error: "custom rmgr not registered"

**Cause:** `shared_preload_libraries` not set or PostgreSQL not restarted.

**Fix:**
1. Verify `postgresql.conf` has `shared_preload_libraries = 'pg_tre'`
2. Restart PostgreSQL (reload is insufficient)

### Error: "could not access file pg_tre"

**Cause:** 1.0.0 binaries not installed or wrong `pg_config` used during build.

**Fix:**
```bash
# Verify pg_config points to the correct PostgreSQL
which pg_config
pg_config --version  # should match your running server

# Rebuild and reinstall
PG_CONFIG=/correct/path/to/pg_config make clean
PG_CONFIG=/correct/path/to/pg_config make install
```

### Existing Queries Slower After Upgrade

**Cause:** Planner incorrectly chooses index scan when seq scan is faster.

**Diagnosis:**
```sql
EXPLAIN (ANALYZE, BUFFERS) 
SELECT * FROM docs WHERE body %~~ tre_pattern('.*foo.*', 3);
-- Check "Index Scan" vs "Seq Scan" in plan
```

**Fix:**
1. Run `ANALYZE` on the table to update statistics
2. If pattern is non-selective, seq scan is correct; use legacy UDF:
   ```sql
   WHERE tre_amatch(body, '.*foo.*', 3)  -- forces seq scan
   ```
3. Adjust cost parameters:
   ```sql
   SET random_page_cost = 1.1;  -- if using SSD
   ```

### ALTER EXTENSION Fails

**Error:** `extension "pg_tre" has no update path from version "0.1.0" to version "1.0.0"`

**Cause:** Upgrade script `sql/pg_tre--0.1.0--1.0.0.sql` not installed.

**Fix:**
```bash
sudo cp sql/pg_tre--0.1.0--1.0.0.sql \
  $(pg_config --sharedir)/extension/
```

Retry:
```sql
ALTER EXTENSION pg_tre UPDATE TO '1.0.0';
```

---

## Testing the Upgrade

Recommended test sequence:

```sql
-- 1. Verify extension version
SELECT extversion FROM pg_extension WHERE extname = 'pg_tre';
-- Should be 1.0.0

-- 2. Test legacy UDFs (backward compat)
SELECT tre_amatch('hello', 'helo', 1);  -- should return true

-- 3. Test new type
SELECT 'hello'::text %~~ tre_pattern('hello', 0);  -- should return true

-- 4. Create test index
CREATE TEMP TABLE test_pg_tre (id serial, body text);
INSERT INTO test_pg_tre (body) VALUES ('PostgreSQL'), ('MySQL'), ('Oracle');
CREATE INDEX test_pg_tre_idx ON test_pg_tre USING tre (body);

-- 5. Test index scan
SET enable_seqscan = off;  -- force index
EXPLAIN SELECT * FROM test_pg_tre WHERE body %~~ tre_pattern('PostgreSQL', 1);
-- Should show "Bitmap Index Scan on test_pg_tre_idx"

-- 6. Verify correctness
SELECT COUNT(*) FROM test_pg_tre WHERE body %~~ tre_pattern('PostgreSQL', 1);
-- Should return 1 (only first row matches)
```

If all tests pass, the upgrade is successful.

---

## Performance Tips

After upgrading to 1.0.0:

1. **Create indexes on columns you query frequently:**
   ```sql
   CREATE INDEX CONCURRENTLY docs_body_tre_idx ON documents USING tre (body);
   ```

2. **Run ANALYZE to populate statistics:**
   ```sql
   ANALYZE documents;
   ```

3. **Tune GUCs for your workload:**
   ```sql
   -- For large pending lists (high write throughput)
   ALTER INDEX docs_body_tre_idx SET (pending_list_limit = 8192);

   -- For better selectivity (more memory)
   ALTER INDEX docs_body_tre_idx SET (bloom_tuple_bits = 256);
   ```

4. **Monitor pending list size:**
   ```sql
   SELECT pg_relation_size('docs_body_tre_idx');  -- bytes
   ```

   If growing rapidly, run `VACUUM` to merge.

5. **Rewrite queries for indexability:**
   - Bad: `WHERE tre_amatch(body, pattern, k)` — always seq scan
   - Good: `WHERE body %~~ tre_pattern(pattern, k)` — uses index

---

## Support

For migration issues:
- File bug reports: https://codeberg.org/gregburd/pg_tre/issues
- Include: PostgreSQL version (`SELECT version();`), pg_tre version, full error message
- Attach: `pg_config --version`, `postgresql.conf` excerpt, EXPLAIN output

For general questions:
- See [doc/pg_tre.md](pg_tre.md) for usage guide
- Check [CHANGELOG.md](../CHANGELOG.md) for what changed
- Review [STATUS.md](../STATUS.md) for known limitations
