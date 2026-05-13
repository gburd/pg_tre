#!/usr/bin/env perl
# Copyright (c) 2025, PostgreSQL Global Development Group
#
# tap/concurrency.pl - Concurrency test for pg_tre.
# Exercises the index under concurrent writers + readers + vacuumer.
# Verifies that index scans always match seq scans even under load.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time sleep);

# Test parameters
my $DURATION = 30;        # seconds to run load
my $N_WRITERS = 8;        # concurrent insert workers
my $N_READERS = 4;        # concurrent query workers
my $VACUUM_INTERVAL = 5;  # seconds between VACUUM runs

my $node = PostgreSQL::Test::Cluster->new('pg_tre_concurrency');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_tre'
max_connections = 50
maintenance_work_mem = '64MB'
checkpoint_timeout = '1h'
});
$node->start;

# Create extension and test table
$node->safe_psql('postgres', 'CREATE EXTENSION pg_tre');
$node->safe_psql('postgres', q{
    CREATE TABLE concurrent_test (
        id serial PRIMARY KEY,
        body text,
        ins_time timestamptz DEFAULT now()
    );
    CREATE INDEX concurrent_idx ON concurrent_test USING tre (body tre_text_ops);
});

# Pattern pool for readers (varied patterns)
my @patterns = (
    'test_\\d+',
    'data[0-9]',
    'worker_[1-8]',
    '[a-z]+_row',
    'concurrent.*',
    'value_\\d\\d',
    'insert_[0-9]+',
    'batch.*data',
    'row_\\d{2,4}',
    'test.*worker'
);

# Start writer processes
my @writer_pids;
for my $w (1..$N_WRITERS) {
    my $pid = $node->background_psql('postgres', on_error_stop => 0);
    push @writer_pids, $pid;
    
    # Send SQL to writer
    my $fh = $pid->{stdin};
    print $fh q{
\set ON_ERROR_STOP 0
DO $$
DECLARE
    start_time timestamptz := clock_timestamp();
    counter int := 0;
BEGIN
    WHILE clock_timestamp() - start_time < interval '30 seconds' LOOP
        INSERT INTO concurrent_test (body)
        VALUES ('worker_} . $w . q{_test_' || counter || '_data' || (random() * 1000)::int);
        counter := counter + 1;
        IF counter % 100 = 0 THEN
            PERFORM pg_sleep(0.001);  -- tiny yield
        END IF;
    END LOOP;
END $$;
};
    print $fh "\\q\n";
}

# Start reader processes
my @reader_pids;
my @reader_errors;
for my $r (1..$N_READERS) {
    my $pid = $node->background_psql('postgres', on_error_stop => 0);
    push @reader_pids, $pid;
    
    my $fh = $pid->{stdin};
    print $fh q{
\set ON_ERROR_STOP 0
DO $$
DECLARE
    start_time timestamptz := clock_timestamp();
    idx_count bigint;
    seq_count bigint;
    pattern text;
    patterns text[] := ARRAY[
        'test_\d+',
        'data[0-9]',
        'worker_[1-8]',
        '[a-z]+_row',
        'concurrent.*',
        'value_\d\d',
        'insert_[0-9]+',
        'batch.*data',
        'row_\d{2,4}',
        'test.*worker'
    ];
BEGIN
    WHILE clock_timestamp() - start_time < interval '30 seconds' LOOP
        pattern := patterns[1 + (random() * array_length(patterns, 1))::int];
        
        -- Index scan
        SET LOCAL enable_seqscan = off;
        SELECT COUNT(*) INTO idx_count
        FROM concurrent_test
        WHERE body %~~ pattern;
        
        -- Seq scan for comparison
        SET LOCAL enable_indexscan = off;
        SET LOCAL enable_bitmapscan = off;
        SELECT COUNT(*) INTO seq_count
        FROM concurrent_test
        WHERE tre_amatch(body, pattern, 0);
        
        IF idx_count != seq_count THEN
            RAISE WARNING 'MISMATCH: pattern=%, index=%, seqscan=%',
                pattern, idx_count, seq_count;
        END IF;
        
        PERFORM pg_sleep(0.1);
    END LOOP;
END $$;
};
    print $fh "\\q\n";
}

# Start vacuumer process
my $vacuum_pid = $node->background_psql('postgres', on_error_stop => 0);
my $vfh = $vacuum_pid->{stdin};
print $vfh qq{
\\set ON_ERROR_STOP 0
DO \$\$
DECLARE
    start_time timestamptz := clock_timestamp();
BEGIN
    WHILE clock_timestamp() - start_time < interval '$DURATION seconds' LOOP
        VACUUM (INDEX_CLEANUP on) concurrent_test;
        PERFORM pg_sleep($VACUUM_INTERVAL);
    END LOOP;
END \$\$;
};
print $vfh "\\q\n";

# Wait for all background processes to complete
note("Waiting for $DURATION seconds of concurrent load...");
foreach my $pid (@writer_pids, @reader_pids, $vacuum_pid) {
    $pid->pump_nb while $pid->pumpable;
    $pid->finish;
}

# Check for reader errors in logs
my $log_content = slurp_file($node->logfile);
my @mismatches = $log_content =~ /WARNING:.*MISMATCH/g;
is(scalar @mismatches, 0, "No index/seqscan mismatches during concurrent load")
    or diag("Found " . scalar(@mismatches) . " mismatches in logs");

# Final differential check: 10 patterns, index must match seq-scan exactly
note("Running final differential check with 10 patterns...");
my $all_match = 1;
for my $i (0..9) {
    my $pattern = $patterns[$i];
    
    my $idx_result = $node->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT array_agg(id ORDER BY id)::text
        FROM concurrent_test
        WHERE body %~~ '$pattern';
    });
    
    my $seq_result = $node->safe_psql('postgres', qq{
        SET enable_indexscan = off;
        SET enable_bitmapscan = off;
        SELECT array_agg(id ORDER BY id)::text
        FROM concurrent_test
        WHERE tre_amatch(body, '$pattern', 0);
    });
    
    if ($idx_result ne $seq_result) {
        fail("Pattern '$pattern': index != seqscan");
        diag("  Index result: " . substr($idx_result // 'NULL', 0, 100));
        diag("  Seq result:   " . substr($seq_result // 'NULL', 0, 100));
        $all_match = 0;
    }
}
ok($all_match, "All 10 patterns match between index and seq-scan");

# Stats check
my $final_count = $node->safe_psql('postgres',
    'SELECT COUNT(*) FROM concurrent_test');
my $indexed_count = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM concurrent_test;
});

note("Final stats: $final_count rows inserted, $indexed_count via index");
is($indexed_count, $final_count, "Index can scan all inserted rows");

$node->stop;
done_testing();
