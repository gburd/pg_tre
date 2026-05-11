#!/usr/bin/env perl
# Copyright (c) 2025, PostgreSQL Global Development Group
#
# Phase 7: pg_upgrade test for pg_tre.
# Since pg_tre only supports PG18+, this test verifies that indexes
# survive a dump/restore cycle (simulating same-major-version upgrade).
#
# If multiple PG versions become available, this can be extended to
# test actual pg_upgrade between minor versions.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# For Phase 7, we only have PG18, so test dump/restore
my $node_old = PostgreSQL::Test::Cluster->new('upgrade_old');

$node_old->init;
$node_old->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_tre'
});
$node_old->start;

# Create test data
$node_old->safe_psql('postgres', 'CREATE EXTENSION pg_tre');
$node_old->safe_psql('postgres', q{
    CREATE TABLE upgrade_test (id serial PRIMARY KEY, t text);
    INSERT INTO upgrade_test (t) SELECT 'data_' || i FROM generate_series(1, 500) i;
    CREATE INDEX upgrade_idx ON upgrade_test USING tre (t tre_text_ops);
});

# Verify index works
my $result = $node_old->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM upgrade_test WHERE t %~~ 'data_1';
});
is($result, '111', 'index works in source database');

# Dump database
my $dump_file = $node_old->backup_dir . '/upgrade_dump.sql';
$node_old->command_ok(
    ['pg_dump', '-f', $dump_file, '-d', 'postgres'],
    'pg_dump succeeds');

$node_old->stop;

# Create new cluster and restore
my $node_new = PostgreSQL::Test::Cluster->new('upgrade_new');
$node_new->init;
$node_new->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_tre'
});
$node_new->start;

# Restore dump
$node_new->command_ok(
    ['psql', '-f', $dump_file, '-d', 'postgres'],
    'restore from dump succeeds');

# Verify index was restored and works
$result = $node_new->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM upgrade_test WHERE t %~~ 'data_1';
});
is($result, '111', 'index works in restored database');

# Verify consistency
my $index_result = $node_new->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM upgrade_test WHERE t %~~ 'data_';
});
my $seq_result = $node_new->safe_psql('postgres', q{
    SET enable_indexscan = off; SET enable_bitmapscan = off;
    SELECT COUNT(*) FROM upgrade_test WHERE tre_amatch(t, 'data_', 0);
});
is($index_result, $seq_result, 'restored index matches seq scan');

# Test that new writes work
$node_new->safe_psql('postgres', q{
    INSERT INTO upgrade_test (t) SELECT 'new_' || i FROM generate_series(1, 100) i;
});

$result = $node_new->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM upgrade_test WHERE t %~~ 'new_';
});
is($result, '100', 'new inserts work after restore');

# Test REINDEX on restored database
$node_new->safe_psql('postgres', 'REINDEX INDEX upgrade_idx;');

$result = $node_new->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM upgrade_test WHERE t %~~ 'data_1';
});
is($result, '111', 'index works after REINDEX in restored database');

$node_new->stop;

# Note: When pg_tre supports multiple PostgreSQL versions, extend this
# test to use actual pg_upgrade binary to upgrade between versions.
# For now, dump/restore is sufficient to verify index portability.

done_testing();
