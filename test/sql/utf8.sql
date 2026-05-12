-- UTF-8 codepoint trigram tests (Phase 3.5)
--
-- Verify that pg_tre correctly indexes and searches multi-byte UTF-8 text.
-- For ASCII-only text, codepoint trigrams are equivalent to byte trigrams
-- (no regression on existing behavior). For multi-byte UTF-8, codepoint
-- trigrams capture character identity correctly.

-- Ensure extension is loaded.
\set ECHO none
\set ON_ERROR_STOP 1
\set VERBOSITY terse

SET client_min_messages = WARNING;

-- Test 1: ASCII-only text (sanity check - should behave exactly as before)
DROP TABLE IF EXISTS utf8_ascii CASCADE;
CREATE TABLE utf8_ascii (id int, txt text);

INSERT INTO utf8_ascii VALUES
    (1, 'hello world'),
    (2, 'goodbye world'),
    (3, 'hello again'),
    (4, 'testing ascii only'),
    (5, 'the quick brown fox');

CREATE INDEX utf8_ascii_idx ON utf8_ascii USING tre (txt);

-- Seq scan baseline
SET enable_indexscan = off;
SET enable_bitmapscan = off;

SELECT id, txt FROM utf8_ascii WHERE txt %~~ 'hello' ORDER BY id;
SELECT id, txt FROM utf8_ascii WHERE txt %~~ 'world' ORDER BY id;
SELECT id, txt FROM utf8_ascii WHERE txt %~~ 'quick' ORDER BY id;

-- Index scan (should match seq scan exactly)
SET enable_indexscan = on;
SET enable_bitmapscan = on;
SET enable_seqscan = off;

SELECT id, txt FROM utf8_ascii WHERE txt %~~ 'hello' ORDER BY id;
SELECT id, txt FROM utf8_ascii WHERE txt %~~ 'world' ORDER BY id;
SELECT id, txt FROM utf8_ascii WHERE txt %~~ 'quick' ORDER BY id;

-- Test 2: CJK characters (multi-byte UTF-8)
DROP TABLE IF EXISTS utf8_cjk CASCADE;
CREATE TABLE utf8_cjk (id int, txt text);

INSERT INTO utf8_cjk VALUES
    (1, '東京は日本の首都です'),
    (2, '北京是中国的首都'),
    (3, '서울은 한국의 수도입니다'),
    (4, 'こんにちは世界'),
    (5, '你好世界'),
    (6, '안녕하세요 세계'),
    (7, '東京タワーは高いです'),
    (8, '北京烤鸭很好吃'),
    (9, '서울 타워는 아름답습니다'),
    (10, '日本語は難しい');

CREATE INDEX utf8_cjk_idx ON utf8_cjk USING tre (txt);

-- Seq scan baseline
SET enable_indexscan = off;
SET enable_bitmapscan = off;

SELECT id, txt FROM utf8_cjk WHERE txt %~~ '東京' ORDER BY id;
SELECT id, txt FROM utf8_cjk WHERE txt %~~ '世界' ORDER BY id;
SELECT id, txt FROM utf8_cjk WHERE txt %~~ '北京' ORDER BY id;
SELECT id, txt FROM utf8_cjk WHERE txt %~~ '서울' ORDER BY id;

-- Index scan (should match seq scan exactly)
SET enable_indexscan = on;
SET enable_bitmapscan = on;
SET enable_seqscan = off;

SELECT id, txt FROM utf8_cjk WHERE txt %~~ '東京' ORDER BY id;
SELECT id, txt FROM utf8_cjk WHERE txt %~~ '世界' ORDER BY id;
SELECT id, txt FROM utf8_cjk WHERE txt %~~ '北京' ORDER BY id;
SELECT id, txt FROM utf8_cjk WHERE txt %~~ '서울' ORDER BY id;

-- Test 3: Accented Latin characters (2-byte UTF-8)
DROP TABLE IF EXISTS utf8_accents CASCADE;
CREATE TABLE utf8_accents (id int, txt text);

INSERT INTO utf8_accents VALUES
    (1, 'café français'),
    (2, 'naïve approach'),
    (3, 'résumé required'),
    (4, 'Zürich is beautiful'),
    (5, 'España is sunny'),
    (6, 'Montréal is cold'),
    (7, 'São Paulo is large'),
    (8, 'Reykjavík is remote'),
    (9, 'Łódź is Polish'),
    (10, 'Malmö is Swedish');

CREATE INDEX utf8_accents_idx ON utf8_accents USING tre (txt);

-- Seq scan baseline
SET enable_indexscan = off;
SET enable_bitmapscan = off;

SELECT id, txt FROM utf8_accents WHERE txt %~~ 'café' ORDER BY id;
SELECT id, txt FROM utf8_accents WHERE txt %~~ 'naïve' ORDER BY id;
SELECT id, txt FROM utf8_accents WHERE txt %~~ 'résumé' ORDER BY id;
SELECT id, txt FROM utf8_accents WHERE txt %~~ 'Zürich' ORDER BY id;

-- Index scan (should match seq scan exactly)
SET enable_indexscan = on;
SET enable_bitmapscan = on;
SET enable_seqscan = off;

SELECT id, txt FROM utf8_accents WHERE txt %~~ 'café' ORDER BY id;
SELECT id, txt FROM utf8_accents WHERE txt %~~ 'naïve' ORDER BY id;
SELECT id, txt FROM utf8_accents WHERE txt %~~ 'résumé' ORDER BY id;
SELECT id, txt FROM utf8_accents WHERE txt %~~ 'Zürich' ORDER BY id;

-- Test 4: Emoji and other 4-byte UTF-8
DROP TABLE IF EXISTS utf8_emoji CASCADE;
CREATE TABLE utf8_emoji (id int, txt text);

INSERT INTO utf8_emoji VALUES
    (1, 'Hello 👋 world'),
    (2, 'I ❤️ PostgreSQL'),
    (3, '🎉 Party time!'),
    (4, 'Fire 🔥 emoji'),
    (5, '🌈 Rainbow colors'),
    (6, '🚀 Rocket launch'),
    (7, '💻 Computer programming'),
    (8, '🍕 Pizza night'),
    (9, '⚡ Lightning fast'),
    (10, '🌟 Star quality');

CREATE INDEX utf8_emoji_idx ON utf8_emoji USING tre (txt);

-- Seq scan baseline
SET enable_indexscan = off;
SET enable_bitmapscan = off;

SELECT id, txt FROM utf8_emoji WHERE txt %~~ 'Hello' ORDER BY id;
SELECT id, txt FROM utf8_emoji WHERE txt %~~ 'PostgreSQL' ORDER BY id;
SELECT id, txt FROM utf8_emoji WHERE txt %~~ 'Party' ORDER BY id;
SELECT id, txt FROM utf8_emoji WHERE txt %~~ 'Fire' ORDER BY id;

-- Index scan (should match seq scan exactly)
SET enable_indexscan = on;
SET enable_bitmapscan = on;
SET enable_seqscan = off;

SELECT id, txt FROM utf8_emoji WHERE txt %~~ 'Hello' ORDER BY id;
SELECT id, txt FROM utf8_emoji WHERE txt %~~ 'PostgreSQL' ORDER BY id;
SELECT id, txt FROM utf8_emoji WHERE txt %~~ 'Party' ORDER BY id;
SELECT id, txt FROM utf8_emoji WHERE txt %~~ 'Fire' ORDER BY id;

-- Test 5: Mixed ASCII and multi-byte
DROP TABLE IF EXISTS utf8_mixed CASCADE;
CREATE TABLE utf8_mixed (id int, txt text);

INSERT INTO utf8_mixed VALUES
    (1, 'Hello 世界'),
    (2, 'Bonjour le monde'),
    (3, 'Hola mundo'),
    (4, 'こんにちは'),
    (5, 'Mix of café and 東京'),
    (6, 'ASCII plus ❤️ emoji'),
    (7, 'Zürich to 北京 flight'),
    (8, 'España loves 🎉'),
    (9, 'Plain ASCII text'),
    (10, 'All 日本語 text');

CREATE INDEX utf8_mixed_idx ON utf8_mixed USING tre (txt);

-- Seq scan baseline
SET enable_indexscan = off;
SET enable_bitmapscan = off;

SELECT id, txt FROM utf8_mixed WHERE txt %~~ 'Hello' ORDER BY id;
SELECT id, txt FROM utf8_mixed WHERE txt %~~ '世界' ORDER BY id;
SELECT id, txt FROM utf8_mixed WHERE txt %~~ 'café' ORDER BY id;
SELECT id, txt FROM utf8_mixed WHERE txt %~~ '東京' ORDER BY id;

-- Index scan (should match seq scan exactly)
SET enable_indexscan = on;
SET enable_bitmapscan = on;
SET enable_seqscan = off;

SELECT id, txt FROM utf8_mixed WHERE txt %~~ 'Hello' ORDER BY id;
SELECT id, txt FROM utf8_mixed WHERE txt %~~ '世界' ORDER BY id;
SELECT id, txt FROM utf8_mixed WHERE txt %~~ 'café' ORDER BY id;
SELECT id, txt FROM utf8_mixed WHERE txt %~~ '東京' ORDER BY id;

-- Test 6: Longer patterns with multi-byte characters
DROP TABLE IF EXISTS utf8_long CASCADE;
CREATE TABLE utf8_long (id int, txt text);

INSERT INTO utf8_long VALUES
    (1, 'The café in Zürich serves great coffee'),
    (2, 'Tokyo tower in 東京 is very tall'),
    (3, 'Naïve résumé writing is not recommended'),
    (4, 'Seoul 서울 has many beautiful parks'),
    (5, 'Beijing 北京 duck is delicious'),
    (6, 'Montréal in winter is très froid'),
    (7, 'España has many fiestas 🎉'),
    (8, 'PostgreSQL ❤️ supports Unicode well'),
    (9, 'Łódź is famous for its film school'),
    (10, 'São Paulo has amazing 🍕 pizza');

CREATE INDEX utf8_long_idx ON utf8_long USING tre (txt);

-- Seq scan baseline
SET enable_indexscan = off;
SET enable_bitmapscan = off;

SELECT id, txt FROM utf8_long WHERE txt %~~ 'Zürich' ORDER BY id;
SELECT id, txt FROM utf8_long WHERE txt %~~ '東京' ORDER BY id;
SELECT id, txt FROM utf8_long WHERE txt %~~ 'Naïve' ORDER BY id;
SELECT id, txt FROM utf8_long WHERE txt %~~ '서울' ORDER BY id;

-- Index scan (should match seq scan exactly)
SET enable_indexscan = on;
SET enable_bitmapscan = on;
SET enable_seqscan = off;

SELECT id, txt FROM utf8_long WHERE txt %~~ 'Zürich' ORDER BY id;
SELECT id, txt FROM utf8_long WHERE txt %~~ '東京' ORDER BY id;
SELECT id, txt FROM utf8_long WHERE txt %~~ 'Naïve' ORDER BY id;
SELECT id, txt FROM utf8_long WHERE txt %~~ '서울' ORDER BY id;

-- Test 7: Invalid UTF-8 should produce clear error during index build
-- Note: PostgreSQL's text type already validates UTF-8, so we can't
-- easily insert invalid UTF-8. This test documents the expected behavior.
--
-- If we somehow had invalid UTF-8 in a text column:
--   CREATE INDEX ... -> ERROR: invalid UTF-8 sequence at byte offset N
--
-- This should never happen in practice because PostgreSQL validates UTF-8
-- at INSERT time (for UTF-8 databases).

-- Cleanup
DROP TABLE utf8_ascii CASCADE;
DROP TABLE utf8_cjk CASCADE;
DROP TABLE utf8_accents CASCADE;
DROP TABLE utf8_emoji CASCADE;
DROP TABLE utf8_mixed CASCADE;
DROP TABLE utf8_long CASCADE;

-- Reset settings
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_seqscan;
RESET client_min_messages;
