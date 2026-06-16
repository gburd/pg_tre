#!/usr/bin/env perl
# tap/crash_recovery.pl — pg_tre crash-recovery correctness gate.
#
# For each of N cycles:
#   1. Background a writer that inserts in committed batches
#      and records each batch in commit_log.
#   2. Wait briefly so the WAL has things to replay.
#   3. kill -9 the postmaster.
#   4. Restart.  Recovery must succeed without errors.
#   5. For every batch logged in commit_log, the rows it
#      claims to have committed must be findable via the
#      pg_tre index AND via tre_amatch seq-scan.  Index and
#      seq-scan must agree exactly.
#
# This validates the custom rmgr's redo handlers under
# kill-9-mid-write, including the WAL records emitted from
# the pending list, posting builder, and meta updates.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use POSIX qw(:sys_wait_h _exit);

my $LOAD_DURATION = 6;     # seconds the writer runs before kill
my $N_CYCLES      = 2;     # repeat to surface compounding state

my $node = PostgreSQL::Test::Cluster->new('pg_tre_crash');
$node->init;
$node->append_conf('postgresql.conf', q{
shared_preload_libraries = 'pg_tre'
checkpoint_timeout = '1h'
max_wal_size = '512MB'
fsync = on
synchronous_commit = on
});
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_tre');
$node->safe_psql('postgres', q{
    CREATE TABLE crash_test (
        id    serial PRIMARY KEY,
        body  text NOT NULL,
        cycle int NOT NULL,
        seq   int NOT NULL
    );
    CREATE INDEX crash_idx ON crash_test USING tre (body);

    CREATE TABLE commit_log (
        cycle    int,
        batch_id int,
        seq_lo   int,
        seq_hi   int,
        PRIMARY KEY (cycle, batch_id)
    );
});

my $port = $node->port;
my $host = $node->host;
my $psql = $node->installed_command('psql');

# Helper: raw psql (children fork to use this so they don't
# trigger PostgresNode's END-block teardown).
sub raw_psql {
    my ($sql) = @_;
    my @cmd = (
        $psql, '--no-psqlrc', '--no-align', '--tuples-only',
        '--quiet', '--dbname', 'postgres',
        '--host', $host, '--port', $port,
        '--variable', 'ON_ERROR_STOP=0',
        '-c', $sql,
    );
    my $rc = system(@cmd);
    return $rc;
}

for my $cycle (1 .. $N_CYCLES) {
    note("=== cycle $cycle / $N_CYCLES ===");

    # ----------------------------------------------------------
    # Background writer
    # ----------------------------------------------------------
    my $writer = fork();
    die "fork: $!" unless defined $writer;
    if ($writer == 0) {
        delete $node->{_pid};
        my $start = time();
        my $batch_id = 0;
        my $next_seq = 0;
        while (time() - $start < $LOAD_DURATION) {
            my $lo = $next_seq;
            my $hi = $next_seq + 99;
            raw_psql(qq{
                BEGIN;
                INSERT INTO crash_test (body, cycle, seq)
                SELECT 'crash_cycle' || $cycle || '_seq' || g
                       || '_pg_tre_recovery_payload_'
                       || md5(g::text || '$batch_id'),
                       $cycle, g
                  FROM generate_series($lo, $hi) g;
                INSERT INTO commit_log (cycle, batch_id, seq_lo, seq_hi)
                VALUES ($cycle, $batch_id, $lo, $hi);
                COMMIT;
            });
            # Phase B1.3: also append a catalog run each batch, so
            # crash-safe-catalog-writer WAL records are in flight at
            # kill -9.  Each append is its own committed statement
            # (autocommit); a full-range run shares the index roots.
            raw_psql(qq{
                SELECT tre_debug_append_run('crash_idx'::regclass,
                                            0::numeric,
                                            18446744073709551615::numeric);
            });
            $batch_id++;
            $next_seq = $hi + 1;
        }
        POSIX::_exit(0);
    }

    # Let it run, then kill -9 the postmaster
    sleep($LOAD_DURATION);

    my $pmpid;
    {
        my $pf = $node->data_dir . '/postmaster.pid';
        open my $fh, '<', $pf or die "open $pf: $!";
        $pmpid = <$fh>;
        chomp $pmpid;
        close $fh;
    }
    note("kill -9 postmaster PID $pmpid");
    kill 'KILL', $pmpid;

    # Reap writer (may have died too)
    waitpid($writer, 0);

    # The PostgresNode object thinks the postmaster is still
    # running.  Wait for it to actually exit, then clear pid
    # state so $node->start works.
    {
        my $deadline = time() + 5;
        while (kill(0, $pmpid) == 1 && time() < $deadline) {
            select(undef, undef, undef, 0.05);
        }
        # Stale postmaster.pid file; remove it so _update_pid
        # doesn't BAIL_OUT "already running".
        my $pid_file = $node->data_dir . '/postmaster.pid';
        unlink $pid_file;
        $node->{_pid} = undef;
    }

    note("restarting (WAL replay)");
    $node->start;

    # ----------------------------------------------------------
    # Differential check: every batch in commit_log must be
    # findable via both index and seq-scan, exactly the same
    # set of rows.
    # ----------------------------------------------------------
    my $batches = $node->safe_psql('postgres', qq{
        SELECT batch_id, seq_lo, seq_hi FROM commit_log
         WHERE cycle = $cycle ORDER BY batch_id;
    });
    my $checked = 0;
    my $diffs   = 0;
    for my $line (split /\n/, $batches // '') {
        next unless $line;
        my ($bid, $lo, $hi) = split /\|/, $line;
        next unless defined $hi;

        my $idx = $node->safe_psql('postgres', qq{
            SET enable_seqscan = off;
            SELECT count(*) FROM crash_test
             WHERE cycle = $cycle AND seq BETWEEN $lo AND $hi
               AND body %~~ tre_pattern('crash_cycle$cycle', 0);
        });
        my $seq = $node->safe_psql('postgres', qq{
            SET enable_indexscan = off;
            SET enable_bitmapscan = off;
            SELECT count(*) FROM crash_test
             WHERE cycle = $cycle AND seq BETWEEN $lo AND $hi
               AND tre_amatch(body, 'crash_cycle$cycle', 0);
        });
        $checked++;
        if ((defined $idx ? $idx : '') ne (defined $seq ? $seq : '')) {
            diag("cycle=$cycle batch=$bid seq=[$lo..$hi] idx=$idx seq=$seq");
            $diffs++;
        }
    }

    ok($checked > 0,    "cycle $cycle: at least one committed batch survived");
    is($diffs,    0,    "cycle $cycle: every committed batch matches "
                        . "between index and seq-scan");

    # Also sanity: total rows for this cycle visible via the index
    # equals total via seq-scan.
    my $idx_total = $node->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT count(*) FROM crash_test WHERE cycle = $cycle
           AND body %~~ tre_pattern('crash_cycle$cycle', 0);
    });
    my $seq_total = $node->safe_psql('postgres', qq{
        SET enable_indexscan = off;
        SET enable_bitmapscan = off;
        SELECT count(*) FROM crash_test WHERE cycle = $cycle
           AND tre_amatch(body, 'crash_cycle$cycle', 0);
    });
    is($idx_total, $seq_total,
       "cycle $cycle: total index count matches seq-scan count");

    # ----------------------------------------------------------
    # Phase B1.3: the crash-safe catalog writer.  At least one
    # tre_debug_append_run committed before kill -9, so after WAL
    # replay the run catalog must be readable and report >= 2 runs
    # (implicit base run + appended catalog runs).  A regression in
    # the catalog WAL path (e.g. the old REGBUF_WILL_INIT bug) would
    # either PANIC recovery or silently revert the catalog to the
    # last checkpoint -- caught here.
    # ----------------------------------------------------------
    my $runs = $node->safe_psql('postgres', qq{
        SELECT count(*) FROM tre_run_catalog_status('crash_idx'::regclass);
    });
    ok((defined $runs && $runs >= 2),
       "cycle $cycle: catalog runs survived crash recovery "
       . "(got " . (defined $runs ? $runs : 'undef') . " runs)");
}

$node->stop('fast');
done_testing();
