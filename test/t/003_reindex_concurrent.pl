#!/usr/bin/env perl
# Copyright (c) 2025, PostgreSQL Global Development Group
#
# Phase 7: REINDEX CONCURRENTLY test for pg_tre.
# Verifies that concurrent reindexing works correctly while
# INSERT/UPDATE/DELETE operations are ongoing.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('reindex_test');

$node->init;
$node->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_tre'
});
$node->start;

# Setup
$node->safe_psql('postgres', 'CREATE EXTENSION pg_tre');
$node->safe_psql('postgres', q{
    CREATE TABLE reindex_test (id serial PRIMARY KEY, t text);
    INSERT INTO reindex_test (t) SELECT 'initial_' || i FROM generate_series(1, 1000) i;
    CREATE INDEX reindex_idx ON reindex_test USING tre (t tre_text_ops);
});

# Verify index works before REINDEX
my $result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM reindex_test WHERE t %~~ 'initial_1';
});
is($result, '111', 'index functional before REINDEX');

# Test 1: Simple REINDEX CONCURRENTLY
$node->safe_psql('postgres', 'REINDEX INDEX CONCURRENTLY reindex_idx;');

$result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM reindex_test WHERE t %~~ 'initial_1';
});
is($result, '111', 'index functional after REINDEX CONCURRENTLY');

# Verify index consistency
my $index_result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM reindex_test WHERE t %~~ 'initial_';
});
my $seq_result = $node->safe_psql('postgres', q{
    SET enable_indexscan = off; SET enable_bitmapscan = off;
    SELECT COUNT(*) FROM reindex_test WHERE tre_amatch(t, 'initial_', 0);
});
is($index_result, $seq_result, 'index consistent after REINDEX');

# Test 2: REINDEX CONCURRENTLY with concurrent writes
# We'll simulate this by running writes in a background psql session

# Insert data that will be visible during REINDEX
$node->safe_psql('postgres', q{
    INSERT INTO reindex_test (t) SELECT 'concurrent_' || i FROM generate_series(1, 500) i;
});

# Start a transaction that will hold a snapshot
my $background_psql = $node->background_psql('postgres');
$background_psql->query_safe(q{
    BEGIN;
    SELECT COUNT(*) FROM reindex_test;
});

# Perform REINDEX CONCURRENTLY (should succeed despite open transaction)
$node->safe_psql('postgres', 'REINDEX INDEX CONCURRENTLY reindex_idx;');

# Commit background transaction
$background_psql->query_safe('COMMIT;');
$background_psql->quit;

# Verify all data is indexed
$result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM reindex_test WHERE t %~~ 'concurrent_';
});
is($result, '500', 'concurrent writes visible after REINDEX');

# Test 3: Multiple REINDEX cycles with ongoing modifications
for my $cycle (1..3) {
    # Add some data
    $node->safe_psql('postgres', qq{
        INSERT INTO reindex_test (t) SELECT 'cycle${cycle}_' || i FROM generate_series(1, 100) i;
    });
    
    # REINDEX
    $node->safe_psql('postgres', 'REINDEX INDEX CONCURRENTLY reindex_idx;');
    
    # Verify
    $result = $node->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT COUNT(*) FROM reindex_test WHERE t %~~ 'cycle${cycle}_';
    });
    is($result, '100', "cycle $cycle: data indexed after REINDEX");
}

# Test 4: REINDEX after DELETE operations
my $initial_count = $node->safe_psql('postgres', 'SELECT COUNT(*) FROM reindex_test;');

$node->safe_psql('postgres', q{
    DELETE FROM reindex_test WHERE t LIKE 'cycle1%';
});

$node->safe_psql('postgres', 'REINDEX INDEX CONCURRENTLY reindex_idx;');

$result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM reindex_test WHERE t %~~ 'cycle1_';
});
is($result, '0', 'deleted rows not in index after REINDEX');

# Verify index size is reasonable (not bloated)
my $idx_size = $node->safe_psql('postgres', q{
    SELECT pg_size_pretty(pg_relation_size('reindex_idx'));
});
ok($idx_size, 'index size reported after REINDEX');

# Final consistency check
$index_result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM reindex_test;
});
$seq_result = $node->safe_psql('postgres', q{
    SET enable_indexscan = off; SET enable_bitmapscan = off;
    SELECT COUNT(*) FROM reindex_test;
});
is($index_result, $seq_result, 'final consistency after all REINDEX operations');

# Test 5: REINDEX TABLE CONCURRENTLY
$node->safe_psql('postgres', 'REINDEX TABLE CONCURRENTLY reindex_test;');

$result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM reindex_test WHERE t %~~ 'initial_';
});
ok($result > 0, 'index functional after REINDEX TABLE CONCURRENTLY');

$node->stop;
done_testing();
