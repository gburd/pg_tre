#!/usr/bin/env perl
# tap/build_memory.pl -- CREATE INDEX USING tre must not hold every row's
# detoasted text for the whole heap scan.
#
# Through 4.3.0 the build callback decompressed each compressed/out-of-line
# value into the statement's PortalContext and never freed it, so the build
# backend grew by the size of the (decompressed) toasted text: a 30 GB corpus
# with half its rows compressed reached ~33 GB RSS and kept growing (issue #1).
# The fix runs each row in a per-row memory context that is reset.
#
# Builds the same index over a table whose every value is compressed, and
# checks the build backend's peak RSS (VmHWM, Linux only) stays far below the
# decompressed text size.  Serial build, so the leader does all the work.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'needs /proc/<pid>/status (Linux)' unless -r "/proc/$$/status";

my $node = PostgreSQL::Test::Cluster->new('pg_tre_build_memory');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
maintenance_work_mem = 64MB
max_parallel_maintenance_workers = 0
});
$node->start;

# ~400 MB of decompressed text that compresses to a few MB, all toasted.
my $rows = 60000;
$node->safe_psql('postgres', qq{
    CREATE EXTENSION pg_tre;
    CREATE TABLE lk (id int, body text);
    ALTER TABLE lk ALTER COLUMN body SET STORAGE EXTENDED;
    INSERT INTO lk SELECT g, repeat('lorem ipsum dolor sit amet ' || g || ' ', 250)
      FROM generate_series(1, $rows) g;
});
my ($compressed, $text_mb) = split /\|/, $node->safe_psql('postgres', q{
    SELECT count(*) FILTER (WHERE pg_column_size(body) < octet_length(body)),
           sum(octet_length(body)) / (1024 * 1024)
      FROM lk});
is($compressed, $rows, "every row is compressed");
cmp_ok($text_mb, '>', 300, "decompressed text is $text_mb MB");

my $out = $node->safe_psql('postgres', q{
    SET client_min_messages = warning;
    CREATE INDEX lk_tre ON lk USING tre (body);
    SELECT (regexp_match(pg_read_file('/proc/' || pg_backend_pid() || '/status'),
                         'VmHWM:\s+(\d+)'))[1]::bigint / 1024});
my ($hwm_mb) = $out =~ /(\d+)\s*$/;
note "build backend peak RSS: $hwm_mb MB for $text_mb MB of decompressed text";
# 4.3.0 peaked at roughly the decompressed text size plus the sort; fixed it
# stays near maintenance_work_mem plus shared memory, independent of the text.
cmp_ok($hwm_mb, '<', $text_mb / 2,
       "build peak RSS ($hwm_mb MB) does not track the toasted text ($text_mb MB)");

# Same answer as a sequential scan.
my $q = q{SELECT count(*) FROM lk WHERE body %~~ tre_pattern('amet 4242 ', 0)};
my $idx = $node->safe_psql('postgres', "SET enable_seqscan = off; $q");
my $seq = $node->safe_psql('postgres',
    "SET enable_indexscan = off; SET enable_bitmapscan = off; $q");
is($idx, $seq, "index agrees with seq scan ($idx rows)");

$node->stop;
done_testing();
