-- test/sql/parser.sql - Phase 3 parser tests
--
-- Test regex parser, AST construction, and tre_pattern type.

-- Load extension
CREATE EXTENSION IF NOT EXISTS pg_tre;

-- Test tre_pattern type I/O
SELECT 'abc'::tre_pattern;
SELECT 'abc@1'::tre_pattern;
SELECT 'abc@2:1,1,1'::tre_pattern;

-- Test tre_pattern constructors
SELECT tre_pattern('hello');
SELECT tre_pattern('world', 1);
SELECT tre_pattern('test', 2, 1, 1, 1);

-- Test parser with simple patterns
SELECT tre_parse_debug('a');
SELECT tre_parse_debug('abc');
SELECT tre_parse_debug('a|b');
SELECT tre_parse_debug('a*');
SELECT tre_parse_debug('a+');
SELECT tre_parse_debug('a?');
SELECT tre_parse_debug('a{3}');
SELECT tre_parse_debug('a{2,5}');
SELECT tre_parse_debug('(ab)+');
SELECT tre_parse_debug('.');
SELECT tre_parse_debug('^start');
SELECT tre_parse_debug('end$');
SELECT tre_parse_debug('[abc]');
SELECT tre_parse_debug('[a-z]');
SELECT tre_parse_debug('[^0-9]');
SELECT tre_parse_debug('a{~1}');

-- Test extraction (stub returns ALWAYS_TRUE for now)
SELECT tre_extract_debug('hello');
SELECT tre_extract_debug('a|b|c');

-- Test %~~ operator with legacy tre_amatch
SELECT 'hello' %~~ tre_pattern('hello', 0);
SELECT 'hello' %~~ tre_pattern('helo', 1);
SELECT 'hello' %~~ tre_pattern('world', 0);

\echo 'Parser tests complete'
