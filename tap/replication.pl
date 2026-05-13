#!/usr/bin/env perl
# tap/replication.pl — pg_tre streaming replication correctness gate.
#
# Stand up a primary + physical replica.  Apply a workload of
# mixed inserts/updates/deletes on the primary.  Wait for WAL
# replay catch-up.  Verify a panel of indexed queries return
# bit-exactly the same rows on both nodes.  Then promote the
# replica and re-verify.
#
# This validates the custom rmgr's redo handlers in
# src/wal/xlog.c (XLOG_PTRE_META_UPDATE,
# XLOG_PTRE_POSTING_INSERT, XLOG_PTRE_POSTING_SPLIT,
# XLOG_PTRE_PENDING_INSERT, XLOG_PTRE_PENDING_DELETE).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $N_INSERTS = 5000;     # rows to insert on the primary
my $N_UPDATES = 500;      # rows to update
my $N_DELETES = 500;      # rows to delete

# ------------------------------------------------------------------
# Set up primary
# ------------------------------------------------------------------
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
    'postgresql.conf', qq{
shared_preload_libraries = 'pg_tre'
max_wal_senders = 4
wal_level = 'replica'
checkpoint_timeout = '1h'
max_wal_size = '512MB'
});
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION pg_tre');
$primary->safe_psql('postgres', q{
    CREATE TABLE repl_test (
        id   serial PRIMARY KEY,
        body text NOT NULL
    );
    CREATE INDEX repl_idx ON repl_test USING tre (body);
});

# ------------------------------------------------------------------
# Take base backup and start replica
# ------------------------------------------------------------------
my $backup = 'repl_backup';
$primary->backup($backup);

my $replica = PostgreSQL::Test::Cluster->new('replica');
$replica->init_from_backup($primary, $backup, has_streaming => 1);
$replica->append_conf('postgresql.conf', q{
shared_preload_libraries = 'pg_tre'
});
$replica->start;

# ------------------------------------------------------------------
# Apply workload on primary (single session, simple SQL — the goal
# is to exercise WAL replay, not parallelism).
# ------------------------------------------------------------------
note("inserting $N_INSERTS rows on primary");
$primary->safe_psql('postgres', qq{
    INSERT INTO repl_test (body)
    SELECT 'replication_test_data_pg_tre_'
           || g || '_hello_world_token_'
           || md5(g::text)
      FROM generate_series(1, $N_INSERTS) g;
});

note("running $N_UPDATES updates");
$primary->safe_psql('postgres', qq{
    UPDATE repl_test
       SET body = body || ' updated_marker'
     WHERE id IN (
         SELECT id FROM repl_test ORDER BY random() LIMIT $N_UPDATES
     );
});

note("running $N_DELETES deletes");
$primary->safe_psql('postgres', qq{
    DELETE FROM repl_test
     WHERE id IN (
         SELECT id FROM repl_test ORDER BY random() LIMIT $N_DELETES
     );
});

# Insert a final marker that is unlikely to collide with random data.
$primary->safe_psql('postgres', q{
    INSERT INTO repl_test (body) VALUES
        ('FINAL_MARKER_replication_done_unique_phrase'),
        ('FINAL_MARKER_replication_done_unique_phrase_two');
});

# ------------------------------------------------------------------
# Wait for replica to catch up
# ------------------------------------------------------------------
$primary->wait_for_replay_catchup($replica);
note("replica caught up");

# ------------------------------------------------------------------
# Row count must match exactly
# ------------------------------------------------------------------
my $primary_count = $primary->safe_psql('postgres',
    'SELECT count(*) FROM repl_test');
my $replica_count = $replica->safe_psql('postgres',
    'SELECT count(*) FROM repl_test');
is($replica_count, $primary_count,
   "row count: replica == primary ($primary_count rows)");

# ------------------------------------------------------------------
# Panel of indexed queries — bit-exact equality required
# ------------------------------------------------------------------
my @patterns = (
    'replication',
    'pg_tre',
    'hello',
    'token',
    'updated_marker',
    'FINAL_MARKER_replication_done',
    'unique_phrase',
);

my $diff_count = 0;
for my $pat (@patterns) {
    my $p = $primary->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT array_agg(id ORDER BY id)::text FROM repl_test
         WHERE body %~~ tre_pattern('$pat', 0);
    });
    my $r = $replica->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT array_agg(id ORDER BY id)::text FROM repl_test
         WHERE body %~~ tre_pattern('$pat', 0);
    });
    if ((defined $p ? $p : '') ne (defined $r ? $r : '')) {
        diag("MISMATCH pat=$pat primary[0..80]="
             . substr($p // 'NULL', 0, 80)
             . " replica[0..80]=" . substr($r // 'NULL', 0, 80));
        $diff_count++;
    }
}
is($diff_count, 0,
   "indexed queries: replica result == primary for "
   . scalar(@patterns) . "-pattern panel");

# ------------------------------------------------------------------
# Replica index must agree with replica seq-scan (proves WAL replay
# rebuilt the index, not just the heap)
# ------------------------------------------------------------------
my $replica_self_diff = 0;
for my $pat (@patterns) {
    my $idx = $replica->safe_psql('postgres', qq{
        SET enable_seqscan = off;
        SELECT count(*) FROM repl_test
         WHERE body %~~ tre_pattern('$pat', 0);
    });
    my $seq = $replica->safe_psql('postgres', qq{
        SET enable_indexscan = off;
        SET enable_bitmapscan = off;
        SELECT count(*) FROM repl_test
         WHERE tre_amatch(body, '$pat', 0);
    });
    if ((defined $idx ? $idx : '') ne (defined $seq ? $seq : '')) {
        diag("REPLICA self-mismatch pat=$pat idx=$idx seq=$seq");
        $replica_self_diff++;
    }
}
is($replica_self_diff, 0,
   "replica index scan == replica seq-scan for every pattern");

# ------------------------------------------------------------------
# Promote replica and re-run a sanity query
# ------------------------------------------------------------------
$primary->stop('fast');           # avoid both nodes writing
note("promoting replica");
$replica->promote;
$replica->poll_query_until('postgres',
    'SELECT NOT pg_is_in_recovery()', 't');

my $promoted_count = $replica->safe_psql('postgres',
    'SELECT count(*) FROM repl_test');
is($promoted_count, $primary_count,
   "promoted replica preserves heap row count");

my $promoted_idx = $replica->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SELECT count(*) FROM repl_test
     WHERE body %~~ tre_pattern('FINAL_MARKER_replication_done', 0);
});
is($promoted_idx, '2', "promoted replica indexed query returns 2 markers");

$replica->stop('fast');
done_testing();
