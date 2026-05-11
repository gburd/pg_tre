#!/usr/bin/env perl
# Copyright (c) 2025, PostgreSQL Global Development Group
#
# Phase 7: streaming replica test for pg_tre.
# Verifies that pg_tre indexes replicate correctly to standby servers
# and that wal_consistency_checking detects no inconsistencies.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize primary with pg_tre
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf(
	'postgresql.conf', qq{
wal_level = 'replica'
max_wal_senders = 4
shared_preload_libraries = 'pg_tre'
wal_consistency_checking = 'pg_tre'
});
$node_primary->start;

# Setup extension and test table
$node_primary->safe_psql('postgres', 'CREATE EXTENSION pg_tre');
$node_primary->safe_psql('postgres', q{
    CREATE TABLE replica_test (id serial PRIMARY KEY, t text);
    INSERT INTO replica_test (t) SELECT 'primary_' || i FROM generate_series(1, 500) i;
});

# Create index on primary
$node_primary->safe_psql('postgres', q{
    CREATE INDEX replica_idx ON replica_test USING tre (t tre_text_ops);
});

# Take backup and create standby
my $backup_name = 'pg_tre_backup';
$node_primary->backup($backup_name);

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name, has_streaming => 1);
$node_standby->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_tre'
wal_consistency_checking = 'pg_tre'
});
$node_standby->start;

# Wait for catchup
$node_primary->wait_for_replay_catchup($node_standby);

# Query index on standby
my $standby_result = $node_standby->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM replica_test WHERE t %~~ 'primary_1';
});
is($standby_result, '11', 'standby can query replicated index');

# Verify standby matches seq scan
my $standby_index = $node_standby->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM replica_test WHERE t %~~ 'primary_[2-9]';
});
my $standby_seq = $node_standby->safe_psql('postgres', q{
    SET enable_indexscan = off; SET enable_bitmapscan = off;
    SELECT COUNT(*) FROM replica_test WHERE tre_amatch(t, 'primary_[2-9]', 0);
});
is($standby_index, $standby_seq, 'standby index matches seq scan');

# Insert more data on primary
$node_primary->safe_psql('postgres', q{
    INSERT INTO replica_test (t) SELECT 'secondary_' || i FROM generate_series(1, 300) i;
});

# Wait for catchup
$node_primary->wait_for_replay_catchup($node_standby);

# Query new data on standby
$standby_result = $node_standby->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM replica_test WHERE t %~~ 'secondary_';
});
is($standby_result, '300', 'standby sees incremental inserts');

# Run VACUUM on primary to merge pending list
$node_primary->safe_psql('postgres', 'VACUUM replica_test;');
$node_primary->wait_for_replay_catchup($node_standby);

# Verify standby after VACUUM
$standby_result = $node_standby->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM replica_test WHERE t %~~ 'secondary_1';
});
is($standby_result, '11', 'standby consistent after VACUUM on primary');

# Check for wal_consistency_checking errors in logs
my $primary_log = slurp_file($node_primary->logfile);
my $standby_log = slurp_file($node_standby->logfile);

unlike($primary_log, qr/PANIC.*wal_consistency_checking/, 
    'no wal_consistency_checking errors on primary');
unlike($standby_log, qr/PANIC.*wal_consistency_checking/, 
    'no wal_consistency_checking errors on standby');

# Test cascading replication: create second standby from first
my $node_standby_2 = PostgreSQL::Test::Cluster->new('standby_2');
$node_standby_2->init_from_backup($node_standby, $backup_name, has_streaming => 1);
$node_standby_2->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_tre'
wal_consistency_checking = 'pg_tre'
});
$node_standby_2->start;

# More writes on primary
$node_primary->safe_psql('postgres', q{
    INSERT INTO replica_test (t) SELECT 'cascading_' || i FROM generate_series(1, 200) i;
});

$node_primary->wait_for_replay_catchup($node_standby);
$node_standby->wait_for_replay_catchup($node_standby_2, $node_primary);

# Verify second standby
my $standby2_result = $node_standby_2->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM replica_test WHERE t %~~ 'cascading_';
});
is($standby2_result, '200', 'cascading standby sees all data');

# Final consistency across all nodes
my $primary_count = $node_primary->safe_psql('postgres', 
    'SELECT COUNT(*) FROM replica_test');
my $standby1_count = $node_standby->safe_psql('postgres', 
    'SELECT COUNT(*) FROM replica_test');
my $standby2_count = $node_standby_2->safe_psql('postgres', 
    'SELECT COUNT(*) FROM replica_test');

is($standby1_count, $primary_count, 'standby 1 matches primary');
is($standby2_count, $primary_count, 'standby 2 matches primary');

$node_primary->stop;
$node_standby->stop;
$node_standby_2->stop;
done_testing();
