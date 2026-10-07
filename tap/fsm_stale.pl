#!/usr/bin/env perl
# tap/fsm_stale.pl -- a stale FSM entry must not wedge INSERT.
#
# The index FSM is not WAL-logged.  Pages VACUUM frees are recorded there,
# the next inserts reuse them (the "used" mark lives only in shared
# buffers), and a crash before the FSM page is flushed brings the "free"
# marks back.  The pending tail is then a live page the FSM calls free.
# When that tail fills, aminsert extends while holding the meta page and
# the tail exclusively, the FSM hands back the tail itself, and an
# unconditional LockBuffer waited on the backend's own LWLock forever --
# LWLock:BufferContent, empty pg_blocking_pids(), immune to cancel and
# terminate, recurring after every restart (field report 2026-09-29).
#
# Checksums are off so hint-bit FPIs cannot restore the FSM on replay;
# bgwriter is off so it cannot flush the FSM first.  Both only make the
# stale state deterministic -- production reaches it with either setting.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pg_tre_fsm_stale');
# Checksums off: PG18 initdb enables them by default, PG17 has no
# --no-data-checksums flag (and checksums are already off there).
$node->init(extra => $node->pg_version >= 18 ? ['--no-data-checksums'] : []);
$node->append_conf('postgresql.conf', q{
autovacuum = off
checkpoint_timeout = '1h'
max_wal_size = '4GB'
bgwriter_lru_maxpages = 0
wal_log_hints = off
});
$node->start;

$node->safe_psql('postgres', q{
    CREATE EXTENSION pg_tre;
    CREATE EXTENSION pg_freespacemap;
    CREATE TABLE t (id serial PRIMARY KEY, body text NOT NULL);
    CREATE INDEX t_tre ON t USING tre (body);
});

my $ins = sub {
    my ($n) = @_;
    $node->safe_psql('postgres', qq{
        INSERT INTO t (body)
        SELECT 'row_' || g || '_' || md5(g::text) || md5((g * 7)::text)
          FROM generate_series(1, $n) g;
    });
};

# 1. Fill the pending list, merge it, and let the free log hand its pages
#    to the FSM (XID-gated: bump the XID, VACUUM again).
$ins->(20000);
$node->safe_psql('postgres', 'VACUUM t');
$node->safe_psql('postgres', 'SELECT txid_current()');
$node->safe_psql('postgres', 'VACUUM t');
my $free = $node->safe_psql('postgres',
    q{SELECT count(*) FROM pg_freespace('t_tre') WHERE avail > 0});
cmp_ok($free, '>', 20, "VACUUM put pending pages in the FSM ($free)");

# 2. Persist the FSM with those pages marked free.
$node->safe_psql('postgres', 'CHECKPOINT');

# 3. Reuse some of them (fewer than are free, so the tail is a reused page),
#    then crash before the FSM's "used" marks reach disk.
$ins->(5000);
$node->stop('immediate');
$node->start;

my $stale = $node->safe_psql('postgres',
    q{SELECT count(*) FROM pg_freespace('t_tre') WHERE avail > 0});
note("FSM entries after crash recovery: $stale");

# 4. Keep inserting until the tail fills and aminsert extends.  Pre-fix
#    this never returns; bound it so a regression fails instead of hanging.
#    Run it in the background so we can look at what it is waiting on.
my $h = $node->background_psql('postgres', on_error_stop => 0);
$h->query_until(qr/started/, q{
    \echo started
    INSERT INTO t (body)
    SELECT 'post_' || g || '_' || md5(g::text) || md5((g * 3)::text)
      FROM generate_series(1, 20000) g;
    \echo inserted
});
my $done = 0;
for (1 .. 120) {
    my $n = $node->safe_psql('postgres', q{
        SELECT count(*) FROM pg_stat_activity
         WHERE query LIKE '%post_%' AND state = 'active'
           AND pid <> pg_backend_pid()});
    if ($n eq '0') { $done = 1; last; }
    sleep 1;
}
my $wait = $node->safe_psql('postgres', q{
    SELECT coalesce(string_agg(wait_event_type || ':' || wait_event
                               || ' blockers=' || pg_blocking_pids(pid)::text,
                               ', '), 'none')
      FROM pg_stat_activity
     WHERE wait_event = 'BufferContent'});
note("BufferContent waiters: $wait");
ok($done, 'INSERT after crash with a stale FSM completes')
  or diag("still running after 120s; waiters: $wait");
is($wait, 'none', 'no backend left waiting on BufferContent');

if (!$done) {
    # The wedged backend ignores cancel/terminate; only a crash clears it.
    $node->stop('immediate');
    done_testing();
    exit 0;
}
$h->quit;

# 5. The index still agrees with a sequential scan.
my $q = q{SELECT count(*) FROM t WHERE body %~~ tre_pattern('post_1999', 0)};
my $idx = $node->safe_psql('postgres', "SET enable_seqscan=off; $q");
my $seq = $node->safe_psql('postgres',
    "SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT count(*) FROM t WHERE body ~ 'post_1999'");
is($idx, $seq, "index agrees with seq-scan ($idx rows)");
cmp_ok($idx, '>', 0, 'index finds post-crash rows');

$node->stop('immediate');
done_testing();
