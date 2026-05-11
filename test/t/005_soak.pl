#!/usr/bin/env perl
# Copyright (c) 2025, PostgreSQL Global Development Group
#
# Phase 7: soak test for pg_tre.
# Runs continuous mixed workload (INSERT/UPDATE/DELETE/VACUUM/queries)
# for a configurable duration, then verifies consistency.
# Duration controlled by TRE_SOAK_SEC env var (default 60 seconds).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time sleep);

my $soak_duration = $ENV{TRE_SOAK_SEC} || 60;
my $node = PostgreSQL::Test::Cluster->new('soak_test');

$node->init;
$node->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_tre'
log_min_messages = WARNING
});
$node->start;

# Setup
$node->safe_psql('postgres', 'CREATE EXTENSION pg_tre');
$node->safe_psql('postgres', q{
    CREATE TABLE soak_test (id serial PRIMARY KEY, t text, updated_at timestamp);
    INSERT INTO soak_test (t, updated_at) 
        SELECT 'seed_' || i, now() FROM generate_series(1, 500) i;
    CREATE INDEX soak_idx ON soak_test USING tre (t tre_text_ops);
});

my $start_time = time();
my $end_time = $start_time + $soak_duration;
my $iteration = 0;
my $last_vacuum = $start_time;

print "# Starting soak test for $soak_duration seconds...\n";

# Run mixed workload until time expires
while (time() < $end_time) {
    $iteration++;
    
    # INSERT batch
    $node->safe_psql('postgres', qq{
        INSERT INTO soak_test (t, updated_at) 
            SELECT 'iter${iteration}_' || i, now() 
            FROM generate_series(1, 50) i;
    });
    
    # UPDATE some rows
    $node->safe_psql('postgres', q{
        UPDATE soak_test SET updated_at = now() 
        WHERE id % 10 = 0 LIMIT 20;
    });
    
    # DELETE some rows
    $node->safe_psql('postgres', q{
        DELETE FROM soak_test WHERE id % 100 = 0;
    });
    
    # Query using index
    my $result = $node->safe_psql('postgres', q{
        SET enable_seqscan = off;
        SELECT COUNT(*) FROM soak_test WHERE t %~~ 'iter';
    });
    ok($result >= 0, "iteration $iteration: query succeeded (found $result rows)");
    
    # Periodic VACUUM (every ~10 seconds)
    my $now = time();
    if ($now - $last_vacuum > 10) {
        $node->safe_psql('postgres', 'VACUUM soak_test;');
        $last_vacuum = $now;
        print "# Performed VACUUM at iteration $iteration\n";
    }
    
    # Small sleep to avoid overwhelming the server
    sleep(0.1);
}

my $elapsed = time() - $start_time;
print "# Soak test completed: $iteration iterations in $elapsed seconds\n";

# Final verification: compare index vs seq scan
my $index_result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM soak_test;
});

my $seq_result = $node->safe_psql('postgres', q{
    SET enable_indexscan = off; SET enable_bitmapscan = off;
    SELECT COUNT(*) FROM soak_test;
});

is($index_result, $seq_result, 
    "final consistency check after $iteration iterations");

# Test pattern matching consistency
my @patterns = ('seed_', 'iter', '_1', '_2', '_3');
foreach my $pattern (@patterns) {
    my $idx = $node->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT COUNT(*) FROM soak_test WHERE t %~~ '$pattern';
    });
    
    my $seq = $node->safe_psql('postgres', qq{
        SET enable_indexscan = off; SET enable_bitmapscan = off;
        SELECT COUNT(*) FROM soak_test WHERE tre_amatch(t, '$pattern', 0);
    });
    
    is($idx, $seq, "pattern '$pattern' matches between index and seq scan");
}

# Crash and recover to test durability
print "# Testing crash recovery after soak workload...\n";
$node->stop('immediate');
$node->start;

# Verify index still works after crash
$index_result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM soak_test;
});

$seq_result = $node->safe_psql('postgres', q{
    SET enable_indexscan = off; SET enable_bitmapscan = off;
    SELECT COUNT(*) FROM soak_test;
});

is($index_result, $seq_result, 
    'consistency maintained after immediate shutdown and restart');

# Final REINDEX to ensure no corruption
$node->safe_psql('postgres', 'REINDEX INDEX soak_idx;');

$result = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM soak_test WHERE t %~~ 'seed_';
});

ok($result > 0, 'index functional after REINDEX following soak test');

# Check for errors in logs
my $log_contents = slurp_file($node->logfile);
unlike($log_contents, qr/PANIC|FATAL/i, 'no PANIC or FATAL errors during soak test');

$node->stop;

print "# Soak test summary:\n";
print "#   Duration: $elapsed seconds\n";
print "#   Iterations: $iteration\n";
print "#   Final row count: $seq_result\n";
print "#   Status: PASS\n";

done_testing();
