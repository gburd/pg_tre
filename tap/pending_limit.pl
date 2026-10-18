#!/usr/bin/env perl
# tap/pending_limit.pl -- pending_list_limit under concurrent load.
#
# With a small pending_list_limit every few INSERTs merge the pending list
# into the trees, so several writers race to merge (one wins the merge
# lock, the rest skip), while readers scan and a vacuumer runs VACUUM
# (which takes the merge lock and strips dead TIDs) and deletes rows.
# Afterwards, in one snapshot, the index (bitmap and plain index scan)
# must return exactly what a sequential scan returns, the next INSERT must
# drain the pending list, no backend may have raised an error, and
# REINDEX must give the same answers.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time);
use POSIX qw(_exit);

my $DURATION  = 20;
my $N_WRITERS = 6;
my $N_READERS = 2;

my $node = PostgreSQL::Test::Cluster->new('pg_tre_pending_limit');
$node->init;
$node->append_conf('postgresql.conf', q{
max_connections = 40
autovacuum = off
checkpoint_timeout = '1h'
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_tre');
$node->safe_psql('postgres', q{
    CREATE TABLE pl (id bigserial PRIMARY KEY, w int, body text NOT NULL);
    INSERT INTO pl (w, body)
    SELECT 0, 'seed row ' || g || ' quick brown fox ' || md5(g::text)
      FROM generate_series(1, 2000) g;
    CREATE INDEX pl_idx ON pl USING tre (body) WITH (pending_list_limit = 64);
});

my ($port, $host) = ($node->port, $node->host);
my $psql = $node->installed_command('psql');

# Raw psql in children, so PostgreSQL::Test::Cluster's END blocks stay in
# the parent.  Returns stdout, or undef (and the error on stderr) on failure.
sub raw_psql
{
	my ($sql) = @_;
	open(my $fh, '-|', $psql, '--no-psqlrc', '--no-align', '--tuples-only',
		'--quiet', '--dbname', 'postgres', '--host', $host, '--port', $port,
		'--variable', 'ON_ERROR_STOP=1', '-c', $sql) or return undef;
	my $out = do { local $/; <$fh> };
	close($fh) or return undef;
	chomp $out if defined $out;
	return $out;
}

my $deadline = time() + $DURATION;
my @kids;
sub spawn
{
	my ($body) = @_;
	my $pid = fork();
	die "fork: $!" unless defined $pid;
	if ($pid == 0)
	{
		delete $node->{_pid};
		_exit($body->() ? 0 : 1);
	}
	push @kids, $pid;
}

for my $w (1 .. $N_WRITERS)
{
	spawn(sub {
		my $fail = 0;
		while (time() < $deadline)
		{
			$fail++ unless defined raw_psql(qq{
				INSERT INTO pl (w, body)
				SELECT $w, 'writer $w row ' || g || ' quick brawn fox '
				       || md5(random()::text)
				  FROM generate_series(1, 40) g});
		}
		return $fail == 0;
	});
}
for my $r (1 .. $N_READERS)
{
	spawn(sub {
		my $fail = 0;
		while (time() < $deadline)
		{
			for my $p ('quick', 'brawn fox', 'writer 3 row', 'seed row 1[0-9]')
			{
				$fail++ unless defined raw_psql(qq{
					SET enable_seqscan = off;
					SELECT count(*) FROM pl WHERE body %~~ tre_pattern('$p', 0)});
			}
		}
		return $fail == 0;
	});
}
spawn(sub {
	my $fail = 0;
	while (time() < $deadline)
	{
		$fail++ unless defined raw_psql(
			q{DELETE FROM pl WHERE id % 13 = 0 AND id > (SELECT max(id) - 3000 FROM pl)});
		$fail++ unless defined raw_psql('VACUUM pl');
		sleep 1;
	}
	return $fail == 0;
});

my $kid_failures = 0;
for my $pid (@kids)
{
	waitpid($pid, 0);
	$kid_failures++ if $? != 0;
}
is($kid_failures, 0, 'no writer, reader or vacuumer hit an error');

my $rows = $node->safe_psql('postgres', 'SELECT count(*) FROM pl');
cmp_ok($rows, '>', 10000, "workload inserted rows ($rows)");
# While a VACUUM holds the merge lock (the vacuumer loops all along) and
# while one INSERT's merge runs, other INSERTs append without merging, so
# under this load the list is not bounded by the limit; it must drain as
# soon as an INSERT runs with nothing else in the way.
my $pending = $node->safe_psql('postgres', q{SELECT pg_tre_index_pending('pl_idx')});
diag("pending list after the concurrent phase: $pending entries");
# 100 rows of ~80 distinct trigrams each take the list past the
# 2730-entry (64 kB) limit whatever it held: the row that crosses it
# merges the whole list, and the rows after it leave less than the limit.
$node->safe_psql('postgres', q{
	INSERT INTO pl (w, body)
	SELECT 0, 'after the load ' || g || ' ' || md5(g::text) || md5((g * 3)::text)
	  FROM generate_series(1, 100) g});
$pending = $node->safe_psql('postgres', q{SELECT pg_tre_index_pending('pl_idx')});
cmp_ok($pending, '<', 2731,
	"an INSERT past the limit drained the list ($pending entries left)");

sub compare
{
	my ($tag) = @_;
	for my $p ('quick', 'brawn fox', 'writer 3 row', 'seed row 1[0-9]', 'zzz')
	{
		my $res = $node->safe_psql('postgres', qq{
			BEGIN ISOLATION LEVEL REPEATABLE READ;
			SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off;
			SET LOCAL enable_bitmapscan = off;
			SELECT count(*) FROM pl WHERE body %~~ tre_pattern('$p', 0);
			SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = on;
			SELECT count(*) FROM pl WHERE body %~~ tre_pattern('$p', 0);
			SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = on;
			SELECT count(*) FROM pl WHERE body %~~ tre_pattern('$p', 0);
			COMMIT;});
		my ($seq, $bm, $ix) = split /\n/, $res;
		ok($seq == $bm && $seq == $ix, "$tag '$p': seq=$seq bitmap=$bm indexscan=$ix");
	}
}
compare('after load');

my $check = $node->safe_psql('postgres',
	q{SELECT count(*) FILTER (WHERE ok = false) FROM pg_tre_index_check('pl_idx', 10)});
is($check, '0', 'pg_tre_index_check finds no disagreement');

$node->safe_psql('postgres', 'VACUUM pl');
compare('after VACUUM');
$node->safe_psql('postgres', 'REINDEX INDEX pl_idx');
compare('after REINDEX');

my $log = slurp_file($node->logfile);
my @errs = grep { /ERROR|PANIC|TRAP|terminated by signal/ } split /\n/, $log;
is(scalar(@errs), 0, 'server log has no errors') or diag(join("\n", @errs[0 .. ($#errs < 9 ? $#errs : 9)]));

$node->stop;
done_testing();
