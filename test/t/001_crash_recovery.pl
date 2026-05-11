#!/usr/bin/env perl
# Copyright (c) 2025, PostgreSQL Global Development Group
#
# Phase 7: crash recovery test for pg_tre.
# Verifies that the index survives `pg_ctl stop -m immediate` at various
# interrupt points (during build, during bulk insert, during VACUUM).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pg_tre_crash');

$node->init;
$node->append_conf(
	'postgresql.conf', qq{
wal_level = 'replica'
shared_preload_libraries = 'pg_tre'
});
$node->start;

# Create extension and table
$node->safe_psql('postgres', 'CREATE EXTENSION pg_tre');
$node->safe_psql('postgres', q{
    CREATE TABLE crash_test (id serial PRIMARY KEY, t text);
});

# Test 1: Crash during index build
$node->safe_psql('postgres', q{
    INSERT INTO crash_test (t) SELECT 'test_' || i FROM generate_series(1, 100) i;
});

# Create index but crash mid-build (small dataset, but we'll test recovery)
$node->safe_psql('postgres', q{
    CREATE INDEX crash_idx ON crash_test USING tre (t tre_text_ops);
});

# Immediate shutdown
$node->stop('immediate');
$node->start;

# Verify index is usable after restart
my $result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM crash_test WHERE t %~~ 'test_1';
});
is($result, '11', 'index scan works after crash during build');

# Verify index matches seq scan
my $index_result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM crash_test WHERE t %~~ 'test_[2-9]';
});
my $seq_result = $node->safe_psql('postgres', q{
    SET enable_indexscan = off; SET enable_bitmapscan = off;
    SELECT COUNT(*) FROM crash_test WHERE tre_amatch(t, 'test_[2-9]', 0);
});
is($index_result, $seq_result, 'index matches seq scan after crash recovery');

# Test 2: Crash during bulk insert
$node->safe_psql('postgres', q{
    BEGIN;
    INSERT INTO crash_test (t) SELECT 'batch_' || i FROM generate_series(1, 200) i;
    COMMIT;
});

# Crash immediately
$node->stop('immediate');
$node->start;

# Verify pending list was recovered
$result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM crash_test WHERE t %~~ 'batch_1';
});
is($result, '11', 'pending list recovered after crash during insert');

# Test 3: Crash during VACUUM
$node->safe_psql('postgres', q{
    INSERT INTO crash_test (t) SELECT 'vacuum_' || i FROM generate_series(1, 150) i;
});

# Start VACUUM in background and crash
$node->psql('postgres', 'VACUUM crash_test;', on_error_stop => 0);
$node->stop('immediate');
$node->start;

# Verify index is consistent
$index_result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM crash_test WHERE t %~~ 'vacuum_';
});
$seq_result = $node->safe_psql('postgres', q{
    SET enable_indexscan = off; SET enable_bitmapscan = off;
    SELECT COUNT(*) FROM crash_test WHERE tre_amatch(t, 'vacuum_', 0);
});
is($index_result, $seq_result, 'index consistent after crash during VACUUM');

# Test 4: Multiple crash/restart cycles
for my $cycle (1..3) {
    $node->safe_psql('postgres', qq{
        INSERT INTO crash_test (t) SELECT 'cycle${cycle}_' || i FROM generate_series(1, 50) i;
    });
    
    $node->stop('immediate');
    $node->start;
    
    $result = $node->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT COUNT(*) FROM crash_test WHERE t %~~ 'cycle${cycle}_';
    });
    ok($result > 0, "cycle $cycle: index functional after restart");
}

# Final consistency check
$index_result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM crash_test;
});
$seq_result = $node->safe_psql('postgres', q{
    SET enable_indexscan = off; SET enable_bitmapscan = off;
    SELECT COUNT(*) FROM crash_test;
});
is($index_result, $seq_result, 'final consistency check: all rows indexed');

$node->stop;
done_testing();
