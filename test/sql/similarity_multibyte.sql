-- similarity_multibyte: the trigram-similarity family must decode every
-- character without writing past its destination.
--
-- Bug (every release from 1.9.0 through 4.1.0): trgm_set() and pos_trgm() in
-- src/query/trgm_similarity.c decoded each character with
--
--     pg_wchar wc;
--     pg_mb2wchar_with_len(p, &wc, clen);
--
-- but pg_mb2wchar_with_len() always writes a terminating 0 after the decoded
-- character -- two pg_wchars into a one-pg_wchar variable.  The extra 4-byte
-- store lands on whatever the compiler placed next to `wc` on the stack.  With
-- this build's frame layout it is harmless; with others it smashes the stack
-- canary and the backend aborts ("stack smashing detected", SIGABRT, server
-- recovery), which is what the reporter hit on SELECT
-- tre_trgm_similarity('foo','foobar') -- plain ASCII, first character.
--
-- Because the overwrite is silent under most layouts, the counts below cannot
-- detect it on their own; the load-bearing check is running this file (or the
-- whole suite) against a server built with -fsanitize=address, which reports
-- the out-of-bounds store deterministically on the unfixed code.  What this
-- file pins in every build is correctness across 1-, 2-, 3- and 4-byte UTF-8
-- characters through all eight SQL entry points that share the two helpers.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_tre;
RESET client_min_messages;

-- The reporter's exact statement, plus empty / identical / disjoint controls.
SELECT round(tre_trgm_similarity('foo','foobar')::numeric, 6) AS foo_foobar;
SELECT tre_trgm_similarity('', '')          AS empty_empty;
SELECT tre_trgm_similarity('', 'x')         AS empty_x;
SELECT tre_trgm_similarity('same', 'same')  AS identical;
SELECT tre_trgm_similarity('abc', 'xyz')    AS disjoint;

-- One sample per UTF-8 width: é (2 bytes), € (3 bytes), 😀 (4 bytes).  The
-- sizes are asserted so a mis-encoded test file cannot pass vacuously.
SELECT octet_length('é') AS b2, octet_length('€') AS b3, octet_length('😀') AS b4;

-- Every entry point routed through trgm_set():
SELECT round(tre_trgm_similarity('café', 'cafés')::numeric, 6)      AS sim_2b;
SELECT round(tre_trgm_similarity('10€ off', '10€ offer')::numeric, 6) AS sim_3b;
SELECT round(tre_trgm_similarity('hi 😀😀', 'hi 😀😀😀')::numeric, 6) AS sim_4b;
SELECT round(tre_trgm_distance('café', 'cafés')::numeric, 6)        AS dist_2b;
SELECT tre_trgm_sim_op('café', 'cafés')                             AS simop_2b;

-- ...and through pos_trgm():
SELECT round(tre_word_similarity('café', 'un café noir')::numeric, 6)        AS ws_2b;
SELECT round(tre_word_similarity('€', 'prix 10€')::numeric, 6)               AS ws_3b;
SELECT round(tre_strict_word_similarity('😀', 'a 😀 b')::numeric, 6)         AS sws_4b;
SELECT tre_word_sim_op('café', 'un café noir')                               AS wsop_2b;
SELECT round(tre_word_dist_op('café', 'un café noir')::numeric, 6)           AS wsdist_2b;
SELECT tre_strict_word_sim_op('😀', 'a 😀 b')                                AS swsop_4b;
SELECT round(tre_strict_word_dist_op('😀', 'a 😀 b')::numeric, 6)            AS swsdist_4b;

-- Identical multibyte strings must score exactly 1 in every width.
SELECT tre_trgm_similarity('é€😀', 'é€😀')        AS self_mixed;
SELECT tre_word_similarity('é€😀', 'é€😀')        AS wself_mixed;

-- Volume: many decodes per call, so a per-character overwrite is exercised
-- thousands of times in one frame.  Result must be exactly 1.
SELECT tre_trgm_similarity(repeat('ab€😀é', 2000), repeat('ab€😀é', 2000)) AS long_self;
