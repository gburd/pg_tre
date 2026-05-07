-- pg_tre 0.1.0 -> 1.0.0 upgrade.
--
-- Adds: tre access method, handler, default opclass.
-- Keeps: all legacy UDFs unchanged.

CREATE FUNCTION tre_handler(internal)
    RETURNS index_am_handler
    AS 'MODULE_PATHNAME', 'tre_handler'
    LANGUAGE C;

CREATE ACCESS METHOD tre
    TYPE INDEX
    HANDLER tre_handler;

COMMENT ON ACCESS METHOD tre IS
    'approximate-regex index (pg_tre)';

CREATE OPERATOR CLASS tre_text_ops
    DEFAULT FOR TYPE text USING tre AS
    STORAGE text;
