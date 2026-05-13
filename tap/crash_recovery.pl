#!/usr/bin/env perl
# Copyright (c) 2025, PostgreSQL Global Development Group
#
# tap/crash_recovery.pl - Crash recovery test for pg_tre.
# Kills postmaster with -9 mid-load and verifies WAL replay correctness.
# Repeats the cycle 3 times to catch compounding state corruption.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(sleep);

# Test parameters
my $LOAD_DURATION = 10;   # seconds before kill -9
my $N_CYCLES = 3;         # repeat crash/restart cycles

my $node = PostgreSQL::Test::Cluster->new('pg_tre_crash');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_tre'
checkpoint_timeout = '1h'
max_wal_size = '2GB'
fsync = on
synchronous_commit = on
});
$node->start;

# Create extension and test table
$node->safe_psql('postgres', 'CREATE EXTENSION pg_tre');
$node->safe_psql('postgres', q{
    CREATE TABLE crash_test (
        id serial PRIMARY KEY,
        body text,
        cycle int,
        seq int,
        commit_time timestamptz DEFAULT now()
    );
    CREATE INDEX crash_idx ON crash_test USING tre (body tre_text_ops);
});

# Track expected state across cycles
my @committed_ranges;  # Array of [cycle, start_seq, end_seq] for committed batches

for my $cycle (1..$N_CYCLES) {
    note("=== Cycle $cycle/$N_CYCLES ===");
    
    # Create a commit tracking table for this cycle
    $node->safe_psql('postgres', qq{
        CREATE TABLE IF NOT EXISTS commit_log_${cycle} (
            batch_id int PRIMARY KEY,
            start_seq int,
            end_seq int,
            committed_at timestamptz DEFAULT now()
        );
    });
    
    # Start background writer
    note("Starting background writer for $LOAD_DURATION seconds...");
    my $writer = $node->background_psql('postgres', on_error_stop => 0);
    
    my $wfh = $writer->{stdin};
    print $wfh qq{
\\set ON_ERROR_STOP 0
DO \$\$
DECLARE
    start_time timestamptz := clock_timestamp();
    seq int := 0;
    batch_id int := 0;
    batch_start int;
    batch_size int := 50;
BEGIN
    WHILE clock_timestamp() - start_time < interval '$LOAD_DURATION seconds' LOOP
        batch_start := seq;
        
        FOR i IN 1..batch_size LOOP
            INSERT INTO crash_test (body, cycle, seq)
            VALUES ('cycle_${cycle}_test_' || seq || '_data_' || (random() * 1000)::int,
                    $cycle, seq);
            seq := seq + 1;
        END LOOP;
        
        -- Log this committed batch
        INSERT INTO commit_log_${cycle} (batch_id, start_seq, end_seq)
        VALUES (batch_id, batch_start, seq - 1);
        
        batch_id := batch_id + 1;
        COMMIT;
        
        PERFORM pg_sleep(0.01);  -- Small delay between batches
    END LOOP;
END \$\$;
};
    print $wfh "\\q\n";
    
    # Let writer run for a bit
    sleep($LOAD_DURATION);
    
    # Kill -9 the postmaster
    note("Sending kill -9 to postmaster...");
    my $pid = $node->safe_psql('postgres', 'SELECT pg_backend_pid()');
    
    # Get postmaster PID (parent of backend)
    my $postmaster_pid;
    if (-f $node->data_dir . "/postmaster.pid") {
        open my $pidfh, '<', $node->data_dir . "/postmaster.pid"
            or die "Cannot read postmaster.pid: $!";
        $postmaster_pid = <$pidfh>;
        chomp $postmaster_pid;
        close $pidfh;
    }
    
    # Forcibly terminate
    if ($postmaster_pid) {
        kill 'KILL', $postmaster_pid;
        note("Killed postmaster PID $postmaster_pid");
    }
    
    # Wait a moment for the kill to take effect
    sleep(1);
    
    # Try to clean up writer if it's still around
    eval {
        $writer->pump_nb while $writer->pumpable;
        $writer->finish;
    };
    
    # Restart and let WAL replay
    note("Restarting node...");
    $node->start;
    
    # Check that we can query
    my $post_crash_count = $node->safe_psql('postgres',
        "SELECT COUNT(*) FROM crash_test WHERE cycle = $cycle");
    note("Post-crash cycle $cycle: $post_crash_count rows");
    
    # Determine which batches were committed based on commit_log
    my $commit_log = $node->safe_psql('postgres', qq{
        SELECT COALESCE(array_agg(batch_id ORDER BY batch_id)::text, 'NONE')
        FROM commit_log_${cycle};
    });
    note("Committed batches in cycle $cycle: $commit_log");
    
    # Get the range of seq numbers that should be visible
    my $expected_ranges = $node->safe_psql('postgres', qq{
        SELECT array_agg(ROW(start_seq, end_seq) ORDER BY batch_id)::text
        FROM commit_log_${cycle};
    });
    
    # Verify that every committed sequence number is findable via index
    note("Running differential check for cycle $cycle...");
    
    # For each committed batch, verify via index and seq-scan
    my $differential_ok = 1;
    my $batch_rows = $node->safe_psql('postgres', qq{
        SELECT batch_id, start_seq, end_seq
        FROM commit_log_${cycle}
        ORDER BY batch_id;
    });
    
    if ($batch_rows) {
        for my $row (split /\n/, $batch_rows) {
            my ($batch_id, $start_seq, $end_seq) = split /\|/, $row;
            
            # Pick a random seq in this batch
            my $test_seq = $start_seq + int(rand($end_seq - $start_seq + 1));
            
            my $idx_count = $node->safe_psql('postgres', qq{
                SET enable_seqscan = off;
                SELECT COUNT(*) FROM crash_test
                WHERE cycle = $cycle AND body %~~ 'cycle_${cycle}_test_${test_seq}_.*';
            });
            
            my $seq_count = $node->safe_psql('postgres', qq{
                SET enable_indexscan = off;
                SET enable_bitmapscan = off;
                SELECT COUNT(*) FROM crash_test
                WHERE cycle = $cycle AND tre_amatch(body, 'cycle_${cycle}_test_${test_seq}_.*', 0);
            });
            
            if ($idx_count ne $seq_count) {
                fail("Cycle $cycle batch $batch_id seq $test_seq: index=$idx_count seq=$seq_count");
                $differential_ok = 0;
            }
        }
    }
    
    ok($differential_ok, "Cycle $cycle: all committed rows visible via index and seq-scan");
    
    # Verify index overall consistency for this cycle
    my $idx_total = $node->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT COUNT(*) FROM crash_test WHERE cycle = $cycle;
    });
    
    is($idx_total, $post_crash_count,
        "Cycle $cycle: index can scan all rows from this cycle");
}

# Final cross-cycle consistency check
note("=== Final cross-cycle consistency check ===");

my $total_rows = $node->safe_psql('postgres',
    'SELECT COUNT(*) FROM crash_test');
my $idx_total = $node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT COUNT(*) FROM crash_test;
});

is($idx_total, $total_rows, "Index can scan all rows across all cycles");

# Verify index matches seq-scan for broad patterns
my @final_patterns = (
    'cycle_1_.*',
    'cycle_2_.*',
    'cycle_3_.*',
    'test_\\d+',
    'data_[0-9]+'
);

my $final_match = 1;
for my $pattern (@final_patterns) {
    my $idx_result = $node->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT COUNT(*) FROM crash_test WHERE body %~~ '$pattern';
    });
    
    my $seq_result = $node->safe_psql('postgres', qq{
        SET enable_indexscan = off;
        SET enable_bitmapscan = off;
        SELECT COUNT(*) FROM crash_test WHERE tre_amatch(body, '$pattern', 0);
    });
    
    if ($idx_result ne $seq_result) {
        fail("Final check pattern '$pattern': index=$idx_result seq=$seq_result");
        $final_match = 0;
    }
}

ok($final_match, "All final patterns match between index and seq-scan");

note("Total rows after $N_CYCLES crash cycles: $total_rows");

$node->stop;
done_testing();
