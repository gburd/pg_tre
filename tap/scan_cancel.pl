#!/usr/bin/env perl
# tap/scan_cancel.pl -- an ERROR raised while an index scan builds the
# pending-list overlay must not crash the backend.
#
# 4.2.0-4.2.2 kept the overlay (PendingOverlay ov) and the candidate map
# in locals that the PG_CATCH block read after siglongjmp without being
# volatile objects.  clang -O2 (the nix/flake build) then called
# overlay_free() with stale argument registers -- the sigjmp_buf address
# -- and free()d a saved register: SIGABRT "free(): invalid pointer" /
# "munmap_chunk(): invalid pointer", taking the whole cluster through
# crash recovery (field report 2026-10-06, pg.ddx.io).  The trigger is
# any ERROR inside the scan's PG_TRY once the overlay is built; the field
# trigger was statement_timeout on a %~~ ORDER BY ... LIMIT plain index
# scan over a large unmerged pending list.  Only the flake build (clang
# -O2 -flto, `nix build .#pg18`) turned the bug into a crash; gcc and a
# plain clang -O2 PGXS build keep the locals in memory.  CI job
# `nix-clang-tap` runs this file against the flake build, where it fails on
# 4.2.2 and passes with the fix; under other builds it still checks that
# the server stays up and that cancelled scans give correct answers.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pg_tre_scan_cancel');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
shared_buffers = 256MB
restart_after_crash = off
});
$node->start;

$node->safe_psql('postgres', q{
    CREATE EXTENSION pg_tre;
    SELECT setseed(0.31);
    CREATE TABLE words AS SELECT w FROM unnest(string_to_array(
      'postgresql mailing list proposals exposing heavyweight lock wait '
      'queue ordering vacuum index replication checkpoint buffer planner '
      'executor statistics parallel worker transaction snapshot commit '
      'abort toast compression partition trigger extension catalog tuple '
      'visibility freeze connection authentication password role policy', ' ')) w;
    CREATE TABLE m (num bigserial PRIMARY KEY, grp int, ds timestamptz,
                    subject text);
    CREATE FUNCTION rw(n int) RETURNS text LANGUAGE sql VOLATILE AS
      $$ SELECT string_agg(w, ' ') FROM
           (SELECT w FROM words ORDER BY random() LIMIT n) x $$;
    INSERT INTO m(grp, ds, subject)
      SELECT g % 7, now() - (g || ' min')::interval,
             'Re: ' || rw(3 + g % 9) || ' ' || md5(g::text)
        FROM generate_series(1, 40000) g;
    SET client_min_messages = warning;
    CREATE INDEX m_tre ON m USING tre (lower(subject));
    -- Unmerged pending list (no VACUUM): every scan builds the overlay.
    INSERT INTO m(grp, ds, subject)
      SELECT g % 7, now() + (g || ' s')::interval,
             'Re: ' || rw(4 + g % 11) || ' ' || md5((g * 7)::text)
        FROM generate_series(1, 30000) g;
});

my $pending = $node->safe_psql('postgres', q{
    SELECT n_pages FROM tre_page_kind_histogram('m_tre')
     WHERE page_kind = 'pending'});
cmp_ok($pending, '>', 100, "pending list has $pending pages");

# Only inspect log lines written from here on (a re-used TAP dir appends).
my $log_start = -s $node->logfile;

# Plain index scan (amgettuple -> knn_build -> tre_compute_candidate_sm),
# multi-word phrase so the overlay is built and the CNF merge runs.
my $q = q{
    SET enable_seqscan = off; SET enable_bitmapscan = off;
    SET statement_timeout = '%dms';
    SELECT count(*) FROM (SELECT num FROM m
      WHERE lower(subject) %%~~ tre_pattern('%s', 0)
      ORDER BY ds DESC LIMIT 50) s;};

my @pats = ('lock wait queue', 'vacuum index', 'postgresql mailing list',
            'heavyweight lock', 'commit abort', 'toast compression',
            'parallel worker transaction');

my ($timeouts, $finished) = (0, 0);
for my $i (1 .. 120)
{
    my $ms  = 1 + ($i * 37) % 60;            # spread the cancel point
    my $pat = $pats[$i % @pats];
    my ($ret, $out, $err) = $node->psql('postgres', sprintf($q, $ms, $pat));
    if ($err =~ /statement timeout/) { $timeouts++ }
    elsif ($ret == 0)                { $finished++ }
    else
    {
        fail("query $i ($ms ms, '$pat') failed unexpectedly: $err");
        last;
    }
}
cmp_ok($timeouts, '>', 10,
       "statement_timeout fired inside the scan $timeouts times "
       . "($finished completed)");

is($node->safe_psql('postgres', 'SELECT 1'), '1', 'server still up');
my $log = substr(slurp_file($node->logfile), $log_start);
unlike($log, qr/terminated by signal/, 'no backend crashed');
unlike($log, qr/invalid pointer|double free|corrupted/,
       'no glibc heap-corruption report');

# The same scan runs to completion afterwards with the right answer.
my $idx = $node->safe_psql('postgres', q{
    SET enable_seqscan = off; SET enable_bitmapscan = off;
    SELECT count(*) FROM m WHERE lower(subject) %~~ tre_pattern('lock wait', 0)});
my $seq = $node->safe_psql('postgres', q{
    SET enable_indexscan = off; SET enable_bitmapscan = off;
    SELECT count(*) FROM m WHERE lower(subject) %~~ tre_pattern('lock wait', 0)});
is($idx, $seq, "index scan after cancels agrees with seq scan ($idx rows)");

$node->stop;
done_testing();
