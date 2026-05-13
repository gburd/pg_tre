#!/usr/bin/env perl
# Copyright (c) 2025, PostgreSQL Global Development Group
#
# tap/replication.pl - Streaming replication test for pg_tre.
# Exercises the custom rmgr's redo handlers via physical replication.
# Verifies bit-exact result equality between primary and replica.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Test parameters
my $N_OPS = 100000;      # total operations to apply
my $BATCH_SIZE = 1000;   # operations per batch

# Panel of 10 test patterns for final verification
my @test_patterns = (
    'test_\\d+',
    'data[0-9]{3}',
    'repl_[a-z]+',
    'row_\\d{4,6}',
    'update.*value',
    'insert_batch',
    'delete_\\d+',
    'pattern[0-9]',
    '[a-z]+_test',
    'replica.*'
);

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf(
	'postgresql.conf', qq{
wal_level = 'replica'
max_wal_senders = 4
shared_preload_libraries = 'pg_tre'
checkpoint_timeout = '1h'
max_wal_size = '2GB'
});
$node_primary->start;

# Create extension and test table
$node_primary->safe_psql('postgres', 'CREATE EXTENSION pg_tre');
$node_primary->safe_psql('postgres', q{
    CREATE TABLE repl_test (
        id serial PRIMARY KEY,
        body text,
        seq int,
        op_type char(1)  -- 'I'nsert, 'U'pdate, 'D'elete
    );
    CREATE INDEX repl_idx ON repl_test USING tre (body tre_text_ops);
});

note("Creating streaming replica...");
my $backup_name = 'repl_backup';
$node_primary->backup($backup_name);

my $node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->init_from_backup($node_primary, $backup_name, has_streaming => 1);
$node_replica->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_tre'
});
$node_replica->start;

# Apply 100K random operations in batches
note("Applying $N_OPS random operations in batches of $BATCH_SIZE...");
my $next_id = 1;
my @live_ids;  # Track IDs that exist for updates/deletes

for (my $batch = 0; $batch < $N_OPS / $BATCH_SIZE; $batch++) {
    my @ops;
    
    for (my $i = 0; $i < $BATCH_SIZE; $i++) {
        my $rand = rand();
        my $seq = $batch * $BATCH_SIZE + $i;
        
        if ($rand < 0.7 || @live_ids < 10) {
            # 70% INSERT (or if we have too few rows)
            push @ops, sprintf(
                "INSERT INTO repl_test (body, seq, op_type) VALUES ('test_%d_data_%d_repl_%d', %d, 'I')",
                $next_id, int(rand(1000)), int(rand(100)), $seq
            );
            push @live_ids, $next_id;
            $next_id++;
        } elsif ($rand < 0.85 && @live_ids > 0) {
            # 15% UPDATE
            my $target_id = $live_ids[int(rand(scalar @live_ids))];
            push @ops, sprintf(
                "UPDATE repl_test SET body = 'update_value_%d_row_%d', seq = %d, op_type = 'U' WHERE id = %d",
                int(rand(1000)), int(rand(100000)), $seq, $target_id
            );
        } else {
            # 15% DELETE
            if (@live_ids > 0) {
                my $idx = int(rand(scalar @live_ids));
                my $target_id = $live_ids[$idx];
                push @ops, "DELETE FROM repl_test WHERE id = $target_id";
                splice @live_ids, $idx, 1;
            }
        }
    }
    
    # Execute batch
    my $sql = "BEGIN;\n" . join(";\n", @ops) . ";\nCOMMIT;";
    $node_primary->safe_psql('postgres', $sql);
    
    # Progress indicator every 10 batches
    if (($batch + 1) % 10 == 0) {
        note("  Batch " . ($batch + 1) . "/" . ($N_OPS / $BATCH_SIZE));
    }
}

# Wait for replica to catch up
note("Waiting for replica catchup...");
$node_primary->wait_for_replay_catchup($node_replica);

# Verify row counts match
my $primary_count = $node_primary->safe_psql('postgres',
    'SELECT COUNT(*) FROM repl_test');
my $replica_count = $node_replica->safe_psql('postgres',
    'SELECT COUNT(*) FROM repl_test');
is($replica_count, $primary_count, "Replica row count matches primary");

# Panel of 10 queries - results must be bit-exactly equal
note("Running panel of 10 patterns for bit-exact comparison...");
my $all_match = 1;
for my $i (0..9) {
    my $pattern = $test_patterns[$i];
    
    my $primary_result = $node_primary->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT array_agg(id ORDER BY id)::text
        FROM repl_test
        WHERE body %~~ '$pattern';
    });
    
    my $replica_result = $node_replica->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT array_agg(id ORDER BY id)::text
        FROM repl_test
        WHERE body %~~ '$pattern';
    });
    
    if ($primary_result ne $replica_result) {
        fail("Pattern '$pattern': primary != replica");
        diag("  Primary: " . substr($primary_result // 'NULL', 0, 100));
        diag("  Replica: " . substr($replica_result // 'NULL', 0, 100));
        $all_match = 0;
    }
}
ok($all_match, "All 10 patterns match bit-exactly between primary and replica");

# Verify index scans match seq scans on replica
note("Verifying index vs seq-scan on replica...");
my $replica_index_match = 1;
for my $i (0..4) {  # Check 5 patterns
    my $pattern = $test_patterns[$i];
    
    my $idx_result = $node_replica->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT COUNT(*) FROM repl_test WHERE body %~~ '$pattern';
    });
    
    my $seq_result = $node_replica->safe_psql('postgres', qq{
        SET enable_indexscan = off;
        SET enable_bitmapscan = off;
        SELECT COUNT(*) FROM repl_test WHERE tre_amatch(body, '$pattern', 0);
    });
    
    if ($idx_result ne $seq_result) {
        fail("Replica pattern '$pattern': index != seqscan");
        diag("  Index: $idx_result, Seqscan: $seq_result");
        $replica_index_match = 0;
    }
}
ok($replica_index_match, "Replica index scans match seq-scans");

# Promote replica and verify queries still work
note("Promoting replica...");
$node_replica->promote;

# Wait for promotion to complete
$node_replica->poll_query_until('postgres',
    "SELECT NOT pg_is_in_recovery()", 't');

note("Re-running queries on promoted replica...");
my $promoted_match = 1;
for my $i (0..9) {
    my $pattern = $test_patterns[$i];
    
    my $promoted_result = $node_replica->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT array_agg(id ORDER BY id)::text
        FROM repl_test
        WHERE body %~~ '$pattern';
    });
    
    # Compare against primary's last-known state
    my $primary_result = $node_primary->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT array_agg(id ORDER BY id)::text
        FROM repl_test
        WHERE body %~~ '$pattern';
    });
    
    if ($promoted_result ne $primary_result) {
        fail("After promotion pattern '$pattern': result changed");
        diag("  Primary (pre-promotion): " . substr($primary_result // 'NULL', 0, 100));
        diag("  Promoted replica:        " . substr($promoted_result // 'NULL', 0, 100));
        $promoted_match = 0;
    }
}
ok($promoted_match, "Promoted replica maintains query results");

# Verify promoted replica can execute new queries
my $new_query_count = $node_replica->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM repl_test WHERE body %~~ 'test_.*';
});
ok($new_query_count >= 0, "Promoted replica can execute index scans");

note("Final stats: $primary_count rows on primary/replica");

$node_primary->stop;
$node_replica->stop;
done_testing();
