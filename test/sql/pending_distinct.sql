-- test/sql/pending_distinct.sql
--
-- aminsert writes one pending entry per DISTINCT trigram of a row, as the
-- bulk build does.  It used to write one per occurrence; nothing reads the
-- position, so repeats were pure waste, and on whole source files (each
-- trigram ~9x) the pending list reached ~24x the size of the text it
-- indexed (field report 2026-09-29: 137 GB of index over 163 MB).

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS pdi;
CREATE TABLE pdi (id serial PRIMARY KEY, body text)
    WITH (autovacuum_enabled = off);
SET client_min_messages = warning;
CREATE INDEX pdi_tre ON pdi USING tre (body);
RESET client_min_messages;

-- 'abcabc...' has three distinct trigrams (abc, bca, cab) and ~30,000
-- occurrences.  One entry each fits a single pending page; one per
-- occurrence took ~90.
INSERT INTO pdi (body) VALUES (repeat('abc', 10000));
SELECT n_pages AS pending_pages
FROM tre_page_kind_histogram('pdi_tre') WHERE page_kind = 'pending';

-- Still found, via the pending overlay and after the merge.
SET enable_seqscan = off;
SELECT count(*) AS found_pending FROM pdi WHERE body ~ 'cabca';
RESET enable_seqscan;
VACUUM pdi;
SET enable_seqscan = off;
SELECT count(*) AS found_merged FROM pdi WHERE body ~ 'cabca';
RESET enable_seqscan;

DROP TABLE pdi;
