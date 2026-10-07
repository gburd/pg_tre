-- test/sql/encoding_matrix.sql
--
-- pg_tre decodes characters in the DATABASE ENCODING, in two places that
-- must agree: the trigram tokenizer (src/util/utf8.c, used by ambuild,
-- aminsert and query extraction) and TRE itself (pg_tre_mbdecode, through
-- patches/tre-mbdecoder.patch).  This runs one generic check in a fresh
-- database per server encoding whose decoding differs:
--
--   single-byte  LATIN1, WIN1252 (0x80-0x9F are letters), KOI8R (Cyrillic
--                order is not alphabetical), SQL_ASCII (any byte, no
--                validation: one byte is one character, as in core)
--   multibyte    UTF8 (1-4 bytes), EUC_JP (incl. SS2 half-width kana and
--                3-byte SS3 JIS X 0212), EUC_JIS_2004, EUC_KR, EUC_CN,
--                EUC_TW (2-byte, and 4-byte plane 2 whose pg_wchar values
--                exceed INT32_MAX), MULE_INTERNAL (2-4 bytes, no conversion
--                to or from UTF8)
--
-- GB18030, SJIS, BIG5, GBK, UHC, JOHAB and SHIFT_JIS_2004 are client-only
-- encodings; CREATE DATABASE refuses them, which is checked first.
--
-- Per database, from a word list with multibyte characters at the start,
-- middle and end of words, the test derives patterns from the data
-- (exact, anchored, '.' in place of each character, a bracket holding a
-- multibyte character, an alternation, and -- at k=1 -- a deletion,
-- substitution and insertion at each character, so edits straddle
-- multibyte characters), plus encoding-specific ranges.  For EVERY pattern:
--   * the index answer equals the sequential-scan answer, and the plan
--     really used the index;
--   * at k=0 the TRE answer equals core's ~ (same regex syntax here);
--   * the pattern finds at least the row it came from.
-- Rows arrive both before the build (ambuild) and after it (aminsert,
-- pending list); the checks repeat after DELETE + VACUUM (pending merge,
-- bulk delete).  Further per-database checks: edit costs count characters
-- (an anchored single edit costs exactly 1, never the byte length; deleting
-- the LAST character is left out: upstream TRE never matches a deletion
-- right before '$', in any encoding -- tre_amatch_cost('cafe','^caf$',1)
-- is NULL),
-- tre_amatch_detail offsets are byte offsets in the database encoding,
-- LIKE / ILIKE / ~ / ~* / = through the operator class agree with a seq
-- scan, and the trigram-similarity functions (which decode with pg_mblen)
-- give exactly the values the same words give in UTF-8.
--
-- Text is printed only as counts and ids; nothing encoding-dependent (or
-- user, OID or path) reaches the expected output.

\set VERBOSITY terse
\set QUIET on
\set home :DBNAME
SET client_min_messages = warning;
SET client_encoding = 'UTF8';
CREATE EXTENSION IF NOT EXISTS pg_tre;

-- Client-only encodings cannot be server encodings.
CREATE DATABASE pgtre_enc_x TEMPLATE template0 ENCODING 'GB18030' LOCALE 'C';
CREATE DATABASE pgtre_enc_x TEMPLATE template0 ENCODING 'SJIS' LOCALE 'C';
CREATE DATABASE pgtre_enc_x TEMPLATE template0 ENCODING 'BIG5' LOCALE 'C';
CREATE DATABASE pgtre_enc_x TEMPLATE template0 ENCODING 'GBK' LOCALE 'C';
CREATE DATABASE pgtre_enc_x TEMPLATE template0 ENCODING 'UHC' LOCALE 'C';
CREATE DATABASE pgtre_enc_x TEMPLATE template0 ENCODING 'JOHAB' LOCALE 'C';
CREATE DATABASE pgtre_enc_x TEMPLATE template0 ENCODING 'SHIFT_JIS_2004' LOCALE 'C';

-- Word lists, in UTF-8 here.  "via" is the encoding the word is shipped
-- in (convert_to here, convert_from there); 'raw' words are hex bytes.
DROP TABLE IF EXISTS pgtre_encw;
CREATE TABLE pgtre_encw (db text, wid int, w text, via text);
INSERT INTO pgtre_encw VALUES
  ('utf8', 1, 'café', 'UTF8'), ('utf8', 2, 'naïve', 'UTF8'),
  ('utf8', 3, '€uro', 'UTF8'), ('utf8', 4, '日本語の', 'UTF8'),
  ('utf8', 5, '😀smile😀', 'UTF8'), ('utf8', 6, 'a𐐀b𐐨c', 'UTF8'),
  ('utf8', 7, 'ωμέγα', 'UTF8'), ('utf8', 8, 'привет', 'UTF8'),
  ('latin1', 1, 'café', 'LATIN1'), ('latin1', 2, 'naïve', 'LATIN1'),
  ('latin1', 3, 'résumé', 'LATIN1'), ('latin1', 4, 'façade', 'LATIN1'),
  ('latin1', 5, 'smörgås', 'LATIN1'), ('latin1', 6, 'Ærø', 'LATIN1'),
  ('latin1', 7, 'ÿesß', 'LATIN1'),
  ('win1252', 1, 'café', 'WIN1252'), ('win1252', 2, '€uro', 'WIN1252'),
  ('win1252', 3, 'œuvre', 'WIN1252'), ('win1252', 4, 'Šibenik', 'WIN1252'),
  ('win1252', 5, '„quote“', 'WIN1252'), ('win1252', 6, 'ƒoo…', 'WIN1252'),
  ('koi8r', 1, 'привет', 'KOI8R'), ('koi8r', 2, 'ёлка', 'KOI8R'),
  ('koi8r', 3, 'Жук', 'KOI8R'), ('koi8r', 4, 'щука', 'KOI8R'),
  ('koi8r', 5, 'abc мир', 'KOI8R'), ('koi8r', 6, 'юла', 'KOI8R'),
  ('eucjp', 1, '日本語の', 'EUC_JP'), ('eucjp', 2, 'テスト', 'EUC_JP'),
  ('eucjp', 3, 'ﾃｽﾄ', 'EUC_JP'), ('eucjp', 4, '丂丄x', 'EUC_JP'),
  ('eucjp', 5, 'ひらがな', 'EUC_JP'), ('eucjp', 6, 'ascii日本', 'EUC_JP'),
  ('eucjis2004', 1, '日本語', 'EUC_JIS_2004'),
  ('eucjis2004', 2, '𠀋x𠀋', 'EUC_JIS_2004'),
  ('eucjis2004', 3, '丂xㇰ', 'EUC_JIS_2004'),
  ('eucjis2004', 4, 'ﾃｽﾄ', 'EUC_JIS_2004'),
  ('euckr', 1, '한국어', 'EUC_KR'), ('euckr', 2, '서울시', 'EUC_KR'),
  ('euckr', 3, '안녕하세요', 'EUC_KR'), ('euckr', 4, 'abc한', 'EUC_KR'),
  ('euccn', 1, '中文字', 'EUC_CN'), ('euccn', 2, '北京市', 'EUC_CN'),
  ('euccn', 3, '你好世界', 'EUC_CN'), ('euccn', 4, 'abc中', 'EUC_CN'),
  ('euctw', 1, '中文字', 'EUC_TW'), ('euctw', 2, '丟兀中', 'EUC_TW'),
  ('euctw', 3, 'abc中', 'EUC_TW'), ('euctw', 4, '乃中文', 'EUC_TW'),
  ('euctw4', 1, '中文字', 'EUC_TW'), ('euctw4', 2, '乂乜亍', 'EUC_TW'),
  ('euctw4', 3, 'a乂b乜', 'EUC_TW'), ('euctw4', 4, '丟兀中', 'EUC_TW'),
  ('mule', 1, 'café', 'LATIN1'), ('mule', 2, 'naïve', 'LATIN1'),
  ('mule', 3, '日本語', 'EUC_JP'), ('mule', 4, 'ﾃｽﾄ', 'EUC_JP'),
  ('mule', 5, '丂丄x', 'EUC_JP'), ('mule', 6, '한국어', 'EUC_KR'),
  ('mule', 7, '乂乜x', 'EUC_TW'), ('mule', 8, 'мир', 'KOI8R'),
  ('sqlascii', 1, 'café', 'UTF8'), ('sqlascii', 2, '日本', 'UTF8'),
  ('sqlascii', 3, '78ff80fe79', 'raw'), ('sqlascii', 4, '41c3', 'raw'),
  ('sqlascii', 5, 'c3c3ab', 'raw');
-- Encoding-specific patterns (ranges and classes of multibyte characters).
DROP TABLE IF EXISTS pgtre_encx;
CREATE TABLE pgtre_encx (db text, p text, k int, via text);
INSERT INTO pgtre_encx VALUES
  ('utf8', '[α-ω]+', 0), ('utf8', 'μ[ά-ώ]γ', 0), ('utf8', '[😀-😂]smile', 0),
  ('utf8', '[^a-z]uro', 0), ('utf8', 'caf[é]', 0), ('utf8', 'caf[^e]', 0),
  ('utf8', 'a[𐐀-𐐧]b', 0), ('utf8', '^.mile', 1), ('utf8', 'привет$', 0),
  ('latin1', '[à-ÿ]', 0), ('latin1', 'caf[éè]', 0), ('latin1', 'r[^a-z]sum', 0),
  ('latin1', 'sm.rg', 0), ('latin1', 'fa[ç]ade', 1),
  ('win1252', '[€]uro', 0), ('win1252', '[Šš]ibenik', 0), ('win1252', '„.*“', 0),
  ('win1252', 'ƒoo.', 0), ('win1252', '[œ-œ]uvre', 1),
  ('koi8r', '[а-я]+', 0), ('koi8r', '[^a-z]ир', 0), ('koi8r', 'щ.ка', 0),
  ('koi8r', '^[ё]лка$', 0), ('koi8r', 'приват', 1),
  ('eucjp', '[ぁ-ん]+', 0), ('eucjp', '[ァ-ン]+', 0), ('eucjp', '[ｱ-ﾝ]+', 0),
  ('eucjp', '日.語', 0), ('eucjp', '[丂-丄]+x', 0), ('eucjp', 'ﾃｽﾄ$', 0),
  ('eucjp', 'テキスト', 1),
  ('eucjis2004', '[𠀋]x', 0), ('eucjis2004', '日.', 0), ('eucjis2004', '^丂', 0),
  ('eucjis2004', 'x[ㇰ-ㇿ]', 0),
  ('euckr', '[가-힝]+', 0), ('euckr', '서.', 0), ('euckr', '안녕하새요', 1),
  ('euccn', '[一-齄]+', 0), ('euccn', '北.', 0), ('euccn', '你好世介', 1),
  ('euctw', '[中-丟]', 0), ('euctw', 'a.c', 0), ('euctw', '丟兀', 0),
  ('euctw4', '[乂-亍]+', 0), ('euctw4', 'a.b', 0), ('euctw4', '^[^a]', 0),
  ('euctw4', '[乂]', 0), ('euctw4', '乂乜亍', 1),
  ('mule', '[日本]+', 0), ('mule', 'caf.', 0), ('mule', 'na.ve', 0),
  ('mule', '[ｱ-ﾝ]+', 0);
INSERT INTO pgtre_encx VALUES ('mule', '乂.x', 0, 'EUC_TW'), ('mule', '[乂-乜]', 0, 'EUC_TW'),
  ('mule', 'м.р', 0, 'KOI8R'), ('mule', '한.어', 1, 'EUC_KR'), ('mule', '丂[丄]x', 0, 'EUC_JP');
INSERT INTO pgtre_encx VALUES
  ('sqlascii', 'caf..$', 0), ('sqlascii', '^...本$', 0), ('sqlascii', '[^a-z][^a-z]$', 0);

-- Similarity of each word against a sentence holding it, in UTF-8: every
-- non-SQL_ASCII database must reproduce these exactly.
SELECT string_agg(db || ':' || wid || ':' ||
         round(tre_trgm_similarity(w, 'pre ' || w || ' post')::numeric, 6) || ':' ||
         round(tre_word_similarity(w, 'pre ' || w || ' post')::numeric, 6) || ':' ||
         round(tre_strict_word_similarity(w, w || ' post')::numeric, 6), ',') AS sims
  FROM pgtre_encw WHERE via <> 'raw' AND db <> 'sqlascii' \gset

-- The per-database check.  Expects :words, :extra and :db.
SELECT $setup$
CREATE EXTENSION pg_tre;
CREATE TABLE w (wid int, w text);
INSERT INTO w
SELECT split_part(x, ':', 1)::int,
       convert_from(decode(split_part(x, ':', 2), 'hex'), split_part(x, ':', 3))
  FROM unnest(string_to_array(:'words', ',')) x;
CREATE TABLE m (id serial, wid int, s text);
INSERT INTO m (wid, s) SELECT wid, w FROM w;
INSERT INTO m (wid, s) SELECT NULL, 'filler ' || g FROM generate_series(1, 200) g;
CREATE INDEX m_tre ON m USING tre (s);
-- after the build: aminsert / pending list
INSERT INTO m (wid, s) SELECT wid, 'pre ' || w || ' post' FROM w;
INSERT INTO m (wid, s) SELECT wid, w || w FROM w;
CREATE TABLE pat (wid int, kind text, p text, k int);
INSERT INTO pat
SELECT wid, 'exact', w, 0 FROM w
UNION ALL SELECT wid, 'anchored', '^' || w || '$', 0 FROM w
UNION ALL SELECT wid, 'dot', overlay(w placing '.' from i for 1), 0
  FROM w, generate_series(1, length(w)) i
UNION ALL SELECT wid, 'bracket', '[' || left(w, 1) || ']' || substr(w, 2), 0 FROM w
UNION ALL SELECT wid, 'alt', '(zzq|' || w || ')', 0 FROM w
UNION ALL SELECT wid, 'del', overlay(w placing '' from i for 1), 1
  FROM w, generate_series(2, length(w) - 1) i
UNION ALL SELECT wid, 'sub', overlay(w placing '#' from i for 1), 1
  FROM w, generate_series(1, length(w)) i
UNION ALL SELECT wid, 'ins', overlay(w placing '#' from i for 0), 1
  FROM w, generate_series(2, length(w)) i
UNION ALL SELECT NULL, 'extra',
       convert_from(decode(split_part(x, ':', 1), 'hex'), split_part(x, ':', 3)),
       split_part(x, ':', 2)::int
  FROM unnest(string_to_array(:'extra', ',')) x;
CREATE FUNCTION chk(p text, k int, OUT idx int[], OUT seq int[], OUT core int[],
                    OUT used bool)
LANGUAGE plpgsql AS $f$
DECLARE plan text;
BEGIN
  PERFORM set_config('enable_seqscan', 'off', true);
  EXECUTE 'EXPLAIN (COSTS OFF) SELECT id FROM m WHERE s %~~ tre_pattern($1, $2)'
    INTO plan USING p, k;
  used := plan NOT LIKE 'Seq Scan%';
  EXECUTE 'SELECT array_agg(id ORDER BY id) FROM m WHERE s %~~ tre_pattern($1, $2)'
    INTO idx USING p, k;
  PERFORM set_config('enable_seqscan', 'on', true);
  PERFORM set_config('enable_indexscan', 'off', true);
  PERFORM set_config('enable_bitmapscan', 'off', true);
  EXECUTE 'SELECT array_agg(id ORDER BY id) FROM m WHERE s %~~ tre_pattern($1, $2)'
    INTO seq USING p, k;
  IF k = 0 THEN
    EXECUTE 'SELECT array_agg(id ORDER BY id) FROM m WHERE s ~ $1' INTO core USING p;
  ELSE
    core := seq;
  END IF;
  PERFORM set_config('enable_indexscan', 'on', true);
  PERFORM set_config('enable_bitmapscan', 'on', true);
END $f$;
CREATE FUNCTION chk_op(op text, p text, OUT idx int[], OUT seq int[], OUT used bool)
LANGUAGE plpgsql AS $f$
DECLARE plan text; q text := format('FROM m WHERE s %s $1', op);
BEGIN
  PERFORM set_config('enable_seqscan', 'off', true);
  EXECUTE 'EXPLAIN (COSTS OFF) SELECT id ' || q INTO plan USING p;
  used := plan NOT LIKE 'Seq Scan%';
  EXECUTE 'SELECT array_agg(id ORDER BY id) ' || q INTO idx USING p;
  PERFORM set_config('enable_seqscan', 'on', true);
  PERFORM set_config('enable_indexscan', 'off', true);
  PERFORM set_config('enable_bitmapscan', 'off', true);
  EXECUTE 'SELECT array_agg(id ORDER BY id) ' || q INTO seq USING p;
  PERFORM set_config('enable_indexscan', 'on', true);
  PERFORM set_config('enable_bitmapscan', 'on', true);
END $f$;
$setup$ AS setup,
$check$
SELECT :'db' AS db, count(*) AS patterns,
       count(*) FILTER (WHERE used) AS via_index,
       coalesce(string_agg(kind || ':' || coalesce(wid, 0), ' ')
                  FILTER (WHERE idx IS DISTINCT FROM seq), 'none') AS idx_ne_seq,
       coalesce(string_agg(kind || ':' || coalesce(wid, 0), ' ')
                  FILTER (WHERE seq IS DISTINCT FROM core), 'none') AS tre_ne_core,
       coalesce(string_agg(kind || ':' || coalesce(wid, 0), ' ')
                  FILTER (WHERE seq IS NULL), 'none') AS found_nothing
  FROM (SELECT kind, wid, (chk(p, k)).* FROM pat) r
$check$ AS check,
$extras$
SELECT :'db' AS db,
       count(*) FILTER (WHERE tre_amatch_cost(w, '^' || p || '$', 1) IS DISTINCT FROM 1)
         AS edit_not_1,
       count(*) AS edits
  FROM (SELECT w, overlay(w placing '' from i for 1) AS p
          FROM w, generate_series(1, length(w) - 1) i
        UNION ALL SELECT w, overlay(w placing '#' from i for 1)
          FROM w, generate_series(1, length(w)) i
        UNION ALL SELECT w, overlay(w placing '#' from i for 0)
          FROM w, generate_series(1, length(w) + 1) i) e;
SELECT :'db' AS db,
       count(*) FILTER (WHERE d.match_start <> 4
                           OR d.match_end <> 4 + octet_length(w.w)) AS bad_offsets,
       count(*) FILTER (WHERE s.cost <> 1 OR s.num_subst <> 1 OR s.match_start <> 0
                           OR s.match_end <> octet_length(w.w)) AS bad_subst_detail
  FROM w,
       LATERAL tre_amatch_detail('pre ' || w.w || ' post', w.w, 0) d,
       LATERAL tre_amatch_detail(w.w, '^' || overlay(w.w placing '#' from 2 for 1) || '$', 1) s;
SELECT :'db' AS db, count(*) AS checks, count(*) FILTER (WHERE used) AS via_index,
       coalesce(string_agg(op || ':' || wid, ' ') FILTER (WHERE idx IS DISTINCT FROM seq),
                'none') AS idx_ne_seq,
       count(*) FILTER (WHERE seq IS NULL) AS found_nothing
  FROM (SELECT op, wid, (chk_op(op, p)).*
          FROM w, LATERAL (VALUES ('~~', '%' || w || '%'), ('~~*', '%' || w || '%'),
                                  ('~', w), ('~*', w), ('=', w), ('~~', w || '_%'))
                                  AS o(op, p)) r;
SELECT :'db' AS db, count(*) AS words,
       count(*) FILTER (WHERE round(tre_trgm_similarity(w, 'pre ' || w || ' post')::numeric, 6)
                                <> split_part(r, ':', 3)::numeric
                           OR round(tre_word_similarity(w, 'pre ' || w || ' post')::numeric, 6)
                                <> split_part(r, ':', 4)::numeric
                           OR round(tre_strict_word_similarity(w, w || ' post')::numeric, 6)
                                <> split_part(r, ':', 5)::numeric) AS sim_ne_utf8
  FROM w JOIN (SELECT split_part(x, ':', 2)::int AS wid, x AS r
                 FROM unnest(string_to_array(:'sims', ',')) x
                WHERE split_part(x, ':', 1) = :'db') ref USING (wid);
SELECT :'db' AS db, count(*) AS words,
       count(*) FILTER (WHERE tre_trgm_similarity(w, w) <> 1
                           OR tre_word_similarity(w, 'pre ' || w) <> 1) AS self_sim_ne_1
  FROM w;
DELETE FROM m WHERE (wid IS NULL AND id % 3 = 0) OR s LIKE 'pre %';
VACUUM m;
$extras$ AS extras
\gset


-- Pattern syntax that reaches every tokenizer and matcher path, in a UTF-8
-- and a LATIN1 database (one decodes multibyte, one is single-byte):
-- escaped metacharacters, \n \t \r, approximate {~k} and {m,n} braces,
-- bracket edge cases, embedded options, POSIX classes, character-code and
-- anchor escapes, and back-references -- which only the backtracking
-- matcher implements.  Index answers must equal the sequential scan; k=0
-- TRE answers must equal core's ~ where both dialects mean the same thing
-- (core= true).  Not compared: TRE has no \u, \m, \M, \y, \A, \Z and
-- reads \< \b as GNU word anchors (core: ARE); and TRE rejects [[.x.]]
-- and [[=x=]].  Multibyte back-references ((éa)\1, x(é)y\1z) are compared
-- with core since patches/tre-upstream-fixes.patch fixed TRE's backtracker.
SELECT $syn$
CREATE TABLE sx (id serial, s text);
INSERT INTO sx (s) VALUES
  ('a.b*c+d?e'), ('x(y)z|w'), ('[br]{ac}e'), ('^dol$lar'), ('back\slash'),
  ('dash-y'), (E'tab\there'), (E'new\nline'), (E'cr\rret'), ('café café'),
  ('naïve naïve'), ('abab'), ('éaéa'), ('aéaé'), ('xéyéz'), ('ÉCOLE école'),
  ('résumé'), ('über'), ('abcabc'), ('aaa'), ('hello world');
INSERT INTO sx (s) SELECT 'filler ' || g FROM generate_series(1, 200) g;
CREATE INDEX sx_tre ON sx USING tre (s);
INSERT INTO sx (s) VALUES ('pre café café post'), ('pre abab post'), ('pre éaéa');
CREATE FUNCTION sx_ids(q text, ix bool) RETURNS int[] LANGUAGE plpgsql AS $f$
DECLARE v int[];
BEGIN
  PERFORM set_config('enable_seqscan', CASE WHEN ix THEN 'off' ELSE 'on' END, true);
  PERFORM set_config('enable_indexscan', CASE WHEN ix THEN 'on' ELSE 'off' END, true);
  PERFORM set_config('enable_bitmapscan', CASE WHEN ix THEN 'on' ELSE 'off' END, true);
  EXECUTE 'SELECT array_agg(id ORDER BY id) FROM sx WHERE ' || q INTO v;
  RETURN v;
EXCEPTION WHEN others THEN RETURN ARRAY[-1];
END $f$;
CREATE FUNCTION sx_core(p text) RETURNS int[] LANGUAGE plpgsql AS $f$
DECLARE v int[];
BEGIN
  SELECT array_agg(id ORDER BY id) INTO v FROM sx WHERE s ~ p;
  RETURN v;
EXCEPTION WHEN others THEN RETURN NULL;
END $f$;
CREATE TABLE sxp (p text, k int, core bool);
INSERT INTO sxp VALUES
  ('a\.b\*c\+d\?e', 0, true), ('x\(y\)z\|w', 0, true), ('\[br\]\{ac\}e', 0, true),
  ('\^dol\$lar', 0, true), ('back\\slash', 0, true), ('dash\-y', 0, true),
  ('tab\there', 0, true), ('new\nline', 0, true), ('cr\rret', 0, true),
  ('café{~1}', 0, false), ('(caf){~1}é', 0, false), ('café{1,2}', 0, true),
  ('caf(é){1,}', 0, true), ('(ab){2}', 0, true), ('a{2,3}', 0, true), ('(éa){2}', 0, true),
  ('[]a]bab', 0, true), ('[^]x]bab', 0, true), ('dash[-]y', 0, true), ('dash[y-]', 0, true),
  ('dash[^-a]y', 0, true), ('[a-]bab', 0, true), ('caf[é]', 0, true), ('caf[^e]', 0, true),
  ('caf[[:alpha:]]', 0, true), ('caf[[.é.]]', 0, false), ('caf[[=e=]]', 0, false),
  ('caf[\w]', 0, true), ('caf[\.]', 0, true), ('[\]]', 0, false),
  ('(?i)CAFÉ', 0, true), ('(?i)école', 0, true), ('(?i)ÜBER', 0, true), ('(?i)[À-Ý]cole', 0, true),
  ('caf\xe9', 0, true), ('caf\u00e9', 0, false), ('\mcafé', 0, false), ('café\M', 0, false),
  ('\ycafé\y', 0, false), ('\Acafé', 0, false), ('café\Z', 0, false), ('\<café', 0, false),
  ('\bcafé', 0, false),
  ('(ab)\1', 0, true), ('(éa)\1', 0, true), ('(aé)\1', 0, true), ('x(é)y\1z', 0, true),
  ('(abc)\1$', 0, true), ('(a)\1\1', 0, true), ('(é)(a)\2\1', 0, true),
  ('café', 2, false), ('caXXé', 2, false), ('résumé', 2, false), ('(rés|xyz)umé', 1, false),
  ('naïve{~2}', 0, false), ('^über$', 1, false), ('hello{~1} world', 0, false);
$syn$ AS synsetup,
$syncheck$
SELECT :'db' AS db, count(*) AS patterns,
       count(*) FILTER (WHERE seq <> ARRAY[-1] AND seq IS NOT NULL) AS matched,
       coalesce(string_agg(p, ' ') FILTER (WHERE seq = ARRAY[-1]), 'none') AS errors,
       coalesce(string_agg(p, ' ') FILTER (WHERE idx IS DISTINCT FROM seq), 'none') AS idx_ne_seq,
       coalesce(string_agg(p, ' ') FILTER (WHERE core AND k = 0 AND
                                            seq IS DISTINCT FROM sx_core(p)), 'none') AS tre_ne_core
  FROM (SELECT p, k, core,
               sx_ids(format('s %%~~ tre_pattern(%L, %s)', p, k), true) AS idx,
               sx_ids(format('s %%~~ tre_pattern(%L, %s)', p, k), false) AS seq
          FROM sxp) r
$syncheck$ AS syncheck
\gset

-- Ship one database's words and patterns as psql variables (:ship \gset).
SELECT $ship$
SELECT (SELECT string_agg(wid || ':' ||
                  CASE WHEN via = 'raw' THEN w ELSE encode(convert_to(w, via), 'hex') END
                  || ':' || CASE WHEN via = 'raw' THEN 'SQL_ASCII' ELSE via END, ',')
          FROM pgtre_encw WHERE db = :'db') AS words,
       (SELECT coalesce(string_agg(encode(convert_to(p, CASE WHEN :'db' = 'sqlascii'
                                            THEN 'UTF8' ELSE coalesce(via, e) END), 'hex')
                                   || ':' || k || ':' ||
                                   CASE WHEN :'db' = 'sqlascii' THEN 'SQL_ASCII'
                                        ELSE coalesce(via, e) END, ','), '78:0:UTF8')
          FROM pgtre_encx, (SELECT min(via) AS e FROM pgtre_encw
                             WHERE db = :'db' AND via <> 'raw') v
         WHERE db = :'db') AS extra
$ship$ AS ship \gset

DROP DATABASE IF EXISTS pgtre_enc;
\set db utf8
:ship \gset
CREATE DATABASE pgtre_enc TEMPLATE template0 ENCODING 'UTF8' LOCALE 'C';
\c pgtre_enc
SET client_min_messages = warning;
SET client_encoding = 'UTF8';
:setup
:check;
:extras
:check;
:synsetup
:syncheck;
\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_enc;

\set db latin1
:ship \gset
CREATE DATABASE pgtre_enc TEMPLATE template0 ENCODING 'LATIN1' LOCALE 'C';
\c pgtre_enc
SET client_min_messages = warning;
SET client_encoding = 'UTF8';
:setup
:check;
:extras
:check;
:synsetup
:syncheck;
\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_enc;

\set db win1252
:ship \gset
CREATE DATABASE pgtre_enc TEMPLATE template0 ENCODING 'WIN1252' LOCALE 'C';
\c pgtre_enc
SET client_min_messages = warning;
SET client_encoding = 'UTF8';
:setup
:check;
:extras
:check;
\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_enc;

\set db koi8r
:ship \gset
CREATE DATABASE pgtre_enc TEMPLATE template0 ENCODING 'KOI8R' LOCALE 'C';
\c pgtre_enc
SET client_min_messages = warning;
SET client_encoding = 'UTF8';
:setup
:check;
:extras
:check;
\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_enc;

\set db eucjp
:ship \gset
CREATE DATABASE pgtre_enc TEMPLATE template0 ENCODING 'EUC_JP' LOCALE 'C';
\c pgtre_enc
SET client_min_messages = warning;
SET client_encoding = 'UTF8';
:setup
:check;
:extras
:check;
\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_enc;

\set db eucjis2004
:ship \gset
CREATE DATABASE pgtre_enc TEMPLATE template0 ENCODING 'EUC_JIS_2004' LOCALE 'C';
\c pgtre_enc
SET client_min_messages = warning;
SET client_encoding = 'UTF8';
:setup
:check;
:extras
:check;
\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_enc;

\set db euckr
:ship \gset
CREATE DATABASE pgtre_enc TEMPLATE template0 ENCODING 'EUC_KR' LOCALE 'C';
\c pgtre_enc
SET client_min_messages = warning;
SET client_encoding = 'UTF8';
:setup
:check;
:extras
:check;
\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_enc;

\set db euccn
:ship \gset
CREATE DATABASE pgtre_enc TEMPLATE template0 ENCODING 'EUC_CN' LOCALE 'C';
\c pgtre_enc
SET client_min_messages = warning;
SET client_encoding = 'UTF8';
:setup
:check;
:extras
:check;
\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_enc;

\set db euctw
:ship \gset
CREATE DATABASE pgtre_enc TEMPLATE template0 ENCODING 'EUC_TW' LOCALE 'C';
\c pgtre_enc
SET client_min_messages = warning;
SET client_encoding = 'UTF8';
:setup
:check;
:extras
:check;
\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_enc;

-- EUC_TW plane-2 characters are 4 bytes (SS2 0x8E ...) and decode to
-- pg_wchar values above INT32_MAX, which pg_tre folds into the positive
-- int32 range (they used to end the tokenizer early or loop forever).
\set db euctw4
:ship \gset
CREATE DATABASE pgtre_enc TEMPLATE template0 ENCODING 'EUC_TW' LOCALE 'C';
\c pgtre_enc
SET client_min_messages = warning;
SET client_encoding = 'UTF8';
:setup
:check;
:extras
:check;
SELECT tre_parse_debug('ab乂cd') IS NOT NULL AS parses,
       tre_amatch('a乂b', '^a.b$', 0) AS dot, tre_amatch('乂', '^[乂-亍]$', 0) AS range,
       tre_amatch('乜', '^[^a]$', 0) AS negated, tre_amatch_cost('a乂b', '^a乜b$', 1) AS cost;
\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_enc;

-- MULE_INTERNAL has no UTF8 conversion: the session stays in its own
-- encoding and everything arrives through convert_from().
\set db mule
:ship \gset
CREATE DATABASE pgtre_enc TEMPLATE template0 ENCODING 'MULE_INTERNAL' LOCALE 'C';
\c pgtre_enc
SET client_min_messages = warning;
SET client_encoding = 'MULE_INTERNAL';
SELECT max(octet_length(convert_from(decode(split_part(x, ':', 2), 'hex'),
                                     split_part(x, ':', 3)))) AS longest_word_bytes
  FROM unnest(string_to_array(:'words', ',')) x;
:setup
:check;
:extras
:check;
\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_enc;

-- SQL_ASCII: bytes 0x80-0xFF are characters of their own, unvalidated.
\set db sqlascii
:ship \gset
CREATE DATABASE pgtre_enc TEMPLATE template0 ENCODING 'SQL_ASCII' LOCALE 'C';
\c pgtre_enc
SET client_min_messages = warning;
SET client_encoding = 'SQL_ASCII';
:setup
:check;
:extras
:check;
SELECT length(s) AS chars, octet_length(s) AS bytes,
       tre_amatch_cost(s, '^caf.$', 1) AS one_byte_dot,
       tre_amatch_cost(s, '^caf..$', 1) AS two_byte_dots
  FROM m WHERE wid = 1 AND s NOT LIKE 'pre %' AND s NOT LIKE '%caf%caf%';
\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_enc;

-- Corrupt text.  The server validates text on input, so an invalid or
-- truncated sequence reaches pg_tre only from damaged data; a
-- binary-coercible bytea -> text cast (superuser, rolled back) fakes that.
-- TRE treats an undecodable subject as no match and an undecodable pattern
-- as invalid; the tokenizer (build, insert, index scan) raises an error.
CREATE DATABASE pgtre_enc TEMPLATE template0 ENCODING 'UTF8' LOCALE 'C';
\c pgtre_enc
SET client_min_messages = warning;
CREATE EXTENSION pg_tre;
\set ON_ERROR_ROLLBACK on
BEGIN;
CREATE CAST (bytea AS text) WITHOUT FUNCTION;
CREATE TABLE bad (id int, s text);
INSERT INTO bad VALUES (1, 'xyz'), (2, 'filler'), (3, 'caf''s');
SELECT octet_length('\x78c328'::bytea::text) AS invalid_len,
       octet_length('\x78c3'::bytea::text) AS truncated_len;
-- subject: invalid (c3 28) and truncated (trailing c3) -> no match
SELECT tre_amatch('\x78c328'::bytea::text, 'x', 0) AS invalid_subject,
       tre_amatch('\x78c3'::bytea::text, 'x', 0) AS truncated_subject,
       tre_amatch('\x78c3'::bytea::text, 'x', 1) AS truncated_subject_k1,
       tre_amatch('\x78f09f98'::bytea::text, 'x', 0) AS truncated_4byte;
-- pattern: invalid -> error; truncated -> TRE keeps going (the partial
-- character becomes one unknown character, so 'x' still has to match)
SELECT tre_amatch('xyz', '\x78c328'::bytea::text, 0) AS invalid_pattern;
SELECT tre_amatch('zzz', '\x78c3'::bytea::text, 0) AS truncated_pattern;
-- the tokenizer: index build, insert into an index, index scan
CREATE TABLE bad2 (id int, s text);
INSERT INTO bad2 VALUES (1, '\x7878c328'::bytea::text);
CREATE INDEX bad2_tre ON bad2 USING tre (s);
CREATE INDEX bad_tre ON bad USING tre (s);
INSERT INTO bad VALUES (5, '\x7878c3'::bytea::text);
INSERT INTO bad VALUES (6, '\x787879c328'::bytea::text);
SET enable_seqscan = off;
SELECT id FROM bad WHERE s %~~ tre_pattern('\x78c328'::bytea::text, 0);
SELECT id FROM bad WHERE s %~~ tre_pattern('\x7879c3'::bytea::text, 0);
SELECT id FROM bad WHERE s %~~ tre_pattern('xyz', 0);
RESET enable_seqscan;
ROLLBACK;
\set ON_ERROR_ROLLBACK off
\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_enc;

CREATE DATABASE pgtre_enc TEMPLATE template0 ENCODING 'EUC_JP' LOCALE 'C';
\c pgtre_enc
SET client_min_messages = warning;
CREATE EXTENSION pg_tre;
\set ON_ERROR_ROLLBACK on
BEGIN;
CREATE CAST (bytea AS text) WITHOUT FUNCTION;
-- a4 20: lead byte, invalid trail; 8f a1: 3-byte SS3 cut short
SELECT tre_amatch('\x78a420'::bytea::text, 'x', 0) AS invalid_subject,
       tre_amatch('\x788fa1'::bytea::text, 'x', 0) AS truncated_subject;
SELECT tre_amatch('xyz', '\x78a420'::bytea::text, 0) AS invalid_pattern;
CREATE TABLE bad (id int, s text);
INSERT INTO bad VALUES (1, '\x78788fa1'::bytea::text);
CREATE INDEX bad_tre ON bad USING tre (s);
ROLLBACK;
\set ON_ERROR_ROLLBACK off

\c :home
SET client_min_messages = warning;
DROP DATABASE pgtre_enc;
DROP TABLE pgtre_encw;
DROP TABLE pgtre_encx;
