-- test/sql/utf8_fuzzy.sql
-- Regression test: approximate (k>0) matching over multibyte UTF-8 text.
--
-- The k>0 tiling spine is codepoint-based (pg_tre_hash_trigram_cp), so a
-- fuzzy query over CJK / accented text produces trigram hashes that match
-- how ambuild hashed the indexed text.  Before the fix the k>0 spine slid
-- over raw UTF-8 bytes and dropped codepoints > 0xFF, silently missing
-- matching rows (false negatives).  The correctness gate below is the
-- index vs. seq-scan differential for multibyte patterns at k>=1.

CREATE EXTENSION IF NOT EXISTS pg_tre;

DROP TABLE IF EXISTS u8_t CASCADE;
CREATE TABLE u8_t (id serial, body text);

INSERT INTO u8_t (body) VALUES
  ('café society'),          -- é U+00E9 (2 bytes)
  ('cafe society'),          -- ascii near-variant (1 edit from café)
  ('naïve approach'),        -- ï U+00EF
  ('日本語のテスト'),          -- CJK
  ('日本語のテキスト'),         -- CJK near-variant
  ('résumé draft'),          -- multiple accents
  ('unrelated ascii text');

CREATE INDEX u8_idx ON u8_t USING tre (body);

-- Exact multibyte (k=0).
SET enable_seqscan = off;
SELECT id FROM u8_t WHERE body %~~ tre_pattern('café', 0) ORDER BY id;
-- Fuzzy multibyte (k=1): 'café' within 1 edit should still find 'café';
-- the whole point is the codepoint-aware spine keeps this correct.
SELECT id FROM u8_t WHERE body %~~ tre_pattern('café', 1) ORDER BY id;
-- CJK fuzzy (k=1).
SELECT id FROM u8_t WHERE body %~~ tre_pattern('日本語のテスト', 1) ORDER BY id;
RESET enable_seqscan;

-- Differential: same queries via seq scan must return identical rows.
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT id FROM u8_t WHERE body %~~ tre_pattern('café', 0) ORDER BY id;
SELECT id FROM u8_t WHERE body %~~ tre_pattern('café', 1) ORDER BY id;
SELECT id FROM u8_t WHERE body %~~ tre_pattern('日本語のテスト', 1) ORDER BY id;
RESET enable_indexscan;
RESET enable_bitmapscan;

-- Edits are counted in characters, whatever the database's LC_CTYPE.
-- Under a C/POSIX ctype TRE used to count bytes: 'café' was 2 edits from
-- 'cafe' and キ (3 bytes) was 3 edits from ス, so the k=1 queries above
-- returned different rows on a C and a C.UTF-8 database.
SELECT tre_amatch_cost('cafe', 'café', 5) AS cost_e_acute,
       tre_amatch_cost('日本語のテキスト', '日本語のテスト', 5) AS cost_cjk,
       tre_amatch_cost('naïve', 'naive', 5) AS cost_i_diaeresis;
-- Match offsets stay byte offsets (callers slice the text by bytes).
SELECT match_start, match_end
  FROM tre_amatch_detail('xx café yy', 'café', 0);

DROP TABLE u8_t CASCADE;
