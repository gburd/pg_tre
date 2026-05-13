#!/usr/bin/env perl
# tap/concurrency.pl — pg_tre concurrency correctness gate.
#
# Spawns N writers + M readers + 1 vacuumer.  Children call psql
# directly via system() rather than going through PostgresNode so
# the END destructors don't fight over the postmaster.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time);
use POSIX qw(:sys_wait_h _exit);

my $DURATION         = 15;   # seconds of mixed load
my $N_WRITERS        = 4;
my $N_READERS        = 2;
my $VACUUM_INTERVAL  = 5;

my $node = PostgreSQL::Test::Cluster->new('pg_tre_concurrency');
$node->init;
$node->append_conf(
    'postgresql.conf', qq{
shared_preload_libraries = 'pg_tre'
max_connections = 30
maintenance_work_mem = '64MB'
checkpoint_timeout = '1h'
});
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_tre');
$node->safe_psql('postgres', q{
    CREATE TABLE concurrent_test (
        id     serial PRIMARY KEY,
        body   text NOT NULL
    );
    CREATE INDEX concurrent_idx ON concurrent_test USING tre (body);
});

my $port = $node->port;
my $host = $node->host;
my $psql = $node->installed_command('psql');

# Helper: run a SQL string via raw psql, return stdout (trimmed)
# or undef on error.  Bypasses PostgresNode entirely so children
# don't trigger END-block teardown.
sub raw_psql {
    my ($sql) = @_;
    my @cmd = (
        $psql, '--no-psqlrc', '--no-align', '--tuples-only',
        '--quiet', '--dbname', 'postgres',
        '--host', $host, '--port', $port,
        '--variable', 'ON_ERROR_STOP=1',
        '-c', $sql,
    );
    open(my $fh, '-|', @cmd) or return undef;
    my $out = do { local $/; <$fh> };
    close($fh) or return undef;
    chomp $out if defined $out;
    return $out;
}

my @patterns = (
    'hello',
    'world',
    'concurrent',
    'pg_tre',
    'test_data',
    'random',
);

# ------------------------------------------------------------------
# Spawn workload
# ------------------------------------------------------------------
my @child_pids;
my $deadline = time() + $DURATION;

for my $w (1 .. $N_WRITERS) {
    my $pid = fork();
    die "fork: $!" unless defined $pid;
    if ($pid == 0) {
        # Disable PostgresNode's END-block cleanup in the child:
        # otherwise the first child to exit shuts down the postmaster
        # and the rest fail with "no such socket".
        delete $node->{_pid};
        while (time() < $deadline) {
            raw_psql(qq{
                INSERT INTO concurrent_test (body)
                SELECT 'worker_${w}_test_data_concurrent_'
                       || g || '_pg_tre_random_hello_world_token'
                  FROM generate_series(1, 50) g;
            });
        }
        POSIX::_exit(0);
    }
    push @child_pids, $pid;
}

for my $r (1 .. $N_READERS) {
    my $pid = fork();
    die "fork: $!" unless defined $pid;
    if ($pid == 0) {
        delete $node->{_pid};
        my $local_mismatches = 0;
        while (time() < $deadline) {
            for my $pat (@patterns) {
                my $idx = raw_psql(qq{
                    SET enable_seqscan = off;
                    SELECT count(*) FROM concurrent_test
                     WHERE body %~~ tre_pattern('$pat', 0);
                });
                my $seq = raw_psql(qq{
                    SET enable_indexscan = off;
                    SET enable_bitmapscan = off;
                    SELECT count(*) FROM concurrent_test
                     WHERE tre_amatch(body, '$pat', 0);
                });
                # Counts are point-in-time snapshots from two
                # different sessions; under concurrent insert load
                # they may legitimately differ by the number of
                # rows inserted between the two SELECTs.  We only
                # flag a mismatch when the seq-scan returns FEWER
                # rows than the index, which would indicate the
                # index has phantom or duplicate TIDs.
                if (defined $idx && defined $seq && $idx ne ''
                    && $seq ne '' && ($idx + 0) > ($seq + 0))
                {
                    $local_mismatches++;
                    warn "PHANTOM idx=$idx seq=$seq pat=$pat\n";
                }
            }
        }
        POSIX::_exit($local_mismatches > 0 ? 1 : 0);
    }
    push @child_pids, $pid;
}

{
    my $pid = fork();
    die "fork: $!" unless defined $pid;
    if ($pid == 0) {
        delete $node->{_pid};
        while (time() < $deadline) {
            raw_psql('VACUUM (INDEX_CLEANUP ON) concurrent_test');
            sleep($VACUUM_INTERVAL);
        }
        POSIX::_exit(0);
    }
    push @child_pids, $pid;
}

# ------------------------------------------------------------------
# Reap and assert
# ------------------------------------------------------------------
my $reader_failures = 0;
for my $pid (@child_pids) {
    waitpid($pid, 0);
    $reader_failures++ if (WEXITSTATUS($?) != 0);
}

is($reader_failures, 0,
   "no concurrent reader saw a phantom (idx > seq) under load");

# ------------------------------------------------------------------
# Final differential check on the quiesced state
# ------------------------------------------------------------------
my $total = $node->safe_psql('postgres',
    'SELECT count(*) FROM concurrent_test');
note("inserted $total rows during load");

my $all_ok = 1;
for my $pat (@patterns) {
    my $idx = $node->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT array_agg(id ORDER BY id)::text FROM concurrent_test
         WHERE body %~~ tre_pattern('$pat', 0);
    });
    my $seq = $node->safe_psql('postgres', qq{
        SET enable_indexscan = off;
        SET enable_bitmapscan = off;
        SELECT array_agg(id ORDER BY id)::text FROM concurrent_test
         WHERE tre_amatch(body, '$pat', 0);
    });
    if ((defined $idx ? $idx : '') ne (defined $seq ? $seq : '')) {
        diag("post-load mismatch: pattern=$pat");
        diag("  idx=" . substr($idx // 'NULL', 0, 80));
        diag("  seq=" . substr($seq // 'NULL', 0, 80));
        $all_ok = 0;
    }
}
ok($all_ok, "post-load index == seq-scan for every pattern in panel");

$node->stop('fast');
done_testing();
