#!/usr/bin/env perl
# tap/sm_short_len.pl -- a corrupt (< 8 byte) stored sparsemap length must
# raise DATA_CORRUPTED, not let sparsemap read past the buffer.
#
# Every serialized sparsemap is at least its 8-byte header; pg_tre stores
# lengths from sm_get_size().  sparsemap's sm_open reads the header
# unconditionally, so a stored length of 1..7 was an over-read and 0 made
# the library trust a header the page never held (found while qualifying
# sparsemap 5.8.1).  posting.c now rejects such lengths before sm_wrap.
#
# Corrupts the sparsemap_bytes field of one posting-leaf page on disk
# (offset: page header 24 + right_link 4, pad 4, min_tid 8, max_tid 8 = 48,
# MAXALIGNed header -> field at byte 24+24 = 48), restarts, and queries.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pg_tre_sm_short');
$node->init(extra => ['--no-data-checksums']);
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;
$node->safe_psql('postgres', q{
    CREATE EXTENSION pg_tre;
    CREATE EXTENSION pageinspect;
    CREATE TABLE t (id int, s text);
    -- One hot trigram over many rows forces out-of-line posting leaves.
    INSERT INTO t SELECT g, 'hotword ' || g FROM generate_series(1, 20000) g;
    CREATE INDEX t_tre ON t USING tre (s);
    CHECKPOINT;
});

# Find a posting_leaf block: page_kind 5 lives in the special space; read
# it via pageinspect raw bytes (opaque is the last MAXALIGN(sizeof) bytes;
# page_kind is its first uint16).
my $nblocks = $node->safe_psql('postgres',
    q{SELECT pg_relation_size('t_tre') / 8192});
my @leaves = split /\n/, $node->safe_psql('postgres', qq{
    SELECT b FROM generate_series(1, $nblocks - 1) b,
                  LATERAL get_raw_page('t_tre', b) p
     WHERE get_byte(p, 8192 - 8) + 256 * get_byte(p, 8192 - 7) = 5
     ORDER BY b});
cmp_ok(scalar @leaves, '>', 0, scalar(@leaves) . " posting_leaf pages");
my $leaf = $leaves[0];

my $off = $node->safe_psql('postgres', q{SELECT 24 + 24});
my $path = $node->data_dir . '/' . $node->safe_psql('postgres',
    q{SELECT pg_relation_filepath('t_tre')});
my $before = $node->safe_psql('postgres', qq{
    SELECT get_byte(p, $off) + 256 * get_byte(p, $off + 1)
      FROM get_raw_page('t_tre', $leaf) p});
cmp_ok($before, '>=', 8, "leaf $leaf stores sparsemap_bytes $before");

for my $bad (0, 5) {
    $node->stop;
    open my $fh, '+<', $path or die "open $path: $!";
    binmode $fh;
    for my $b (@leaves) {        # every leaf: whichever the scan reads
        seek $fh, $b * 8192 + $off, 0;
        print $fh pack('V', $bad);
    }
    close $fh;
    $node->start;

    my ($ret, $out, $err) = $node->psql('postgres', q{
        SET enable_seqscan = off;
        SELECT count(*) FROM t WHERE s ~ 'hotword';});
    isnt($ret, 0, "length $bad: query fails");
    like($err, qr/corrupt leaf sparsemap length $bad/,
         "length $bad: DATA_CORRUPTED, not a crash");
    is($node->safe_psql('postgres', 'SELECT 1'), '1',
       "length $bad: server still up");
}

$node->stop;
done_testing();
