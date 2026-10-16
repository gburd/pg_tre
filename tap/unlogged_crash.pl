#!/usr/bin/env perl
# tap/unlogged_crash.pl -- an UNLOGGED tre index survives a crash.
#
# Crash recovery resets an UNLOGGED relation by copying its init fork over
# the main fork, so the init fork (written by ambuildempty) must be
# WAL-logged.  Until 4.4.0 pg_tre wrote it through generic WAL, which logs
# nothing for an UNLOGGED relation: a crash before the next checkpoint left
# a zeroed template, every query on the index then failed "meta page magic
# mismatch (got 0x00000000)" and an INSERT hung on its own meta-page lock.
#
# Here: CREATE INDEX (and REINDEX, which builds a new init fork), crash with
# no checkpoint in between, restart; the reset index must be empty, take
# inserts, and agree with a sequential scan.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pg_tre_unlogged');
$node->init;
$node->append_conf('postgresql.conf', q{
checkpoint_timeout = '1h'
max_wal_size = '1GB'
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_tre');

sub counts
{
	my ($pat) = @_;
	my $idx = $node->safe_psql('postgres', qq{
		SET enable_seqscan = off;
		SELECT count(*) FROM u WHERE s %~~ tre_pattern('$pat', 0)});
	my $seq = $node->safe_psql('postgres', qq{
		SET enable_indexscan = off; SET enable_bitmapscan = off;
		SELECT count(*) FROM u WHERE s %~~ tre_pattern('$pat', 0)});
	return ($idx, $seq);
}

for my $how ('CREATE INDEX', 'REINDEX')
{
	$node->safe_psql('postgres', 'CHECKPOINT');
	if ($how eq 'CREATE INDEX')
	{
		$node->safe_psql('postgres', q{
			SET client_min_messages = warning;
			CREATE UNLOGGED TABLE u (id int, s text);
			INSERT INTO u SELECT g, 'row ' || g FROM generate_series(1, 500) g;
			CREATE INDEX u_idx ON u USING tre (s);
			INSERT INTO u VALUES (501, 'row 501 pending');});
	}
	else
	{
		$node->safe_psql('postgres', q{
			SET client_min_messages = warning;
			INSERT INTO u SELECT g, 'row ' || g FROM generate_series(1, 500) g;
			REINDEX INDEX u_idx;});
	}

	$node->stop('immediate');
	$node->start;

	my ($idx, $seq) = counts('row');
	is($idx, '0', "$how, crash: reset index is empty and readable");
	is($seq, '0', "$how, crash: reset heap is empty");

	$node->safe_psql('postgres', q{
		INSERT INTO u SELECT g, 'row ' || g || ' again' FROM generate_series(1, 300) g;});
	($idx, $seq) = counts('row');
	is($idx, '300', "$how, crash: inserts after reset are indexed");
	is($idx, $seq, "$how, crash: index agrees with seq scan");
	($idx, $seq) = counts('again');
	is($idx, $seq, "$how, crash: index agrees with seq scan ('again')");
}

$node->stop;
done_testing();
