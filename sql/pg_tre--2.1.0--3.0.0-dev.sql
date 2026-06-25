-- pg_tre 2.1.0 -> 3.0.0-dev upgrade.
--
-- This stub is filled in as catalog-changing features land in
-- the dev cycle.  At release time, run the audit from
-- RELEASING.md to confirm every new CREATE FUNCTION /
-- OPERATOR / OPERATOR CLASS / TYPE / CAST / ALTER OPERATOR
-- FAMILY in sql/pg_tre--3.0.0-dev.sql has a matching statement
-- here, and every removed/renamed object has a matching
-- DROP / ALTER.

-- 3.0.0: drop the (text,text) similarity operators that collide with
-- pg_trgm, so pg_tre and pg_trgm can be loaded in the same database.
-- The distinctly-named functions remain (tre_trgm_similarity,
-- tre_word_similarity, tre_trgm_sim_op, tre_word_dist_op, ...); callers
-- use those, or load pg_trgm for its % / <-> / <% / etc.  pg_tre's own
-- %~~ and <@> operators are unaffected.
DROP OPERATOR IF EXISTS % (text, text);
DROP OPERATOR IF EXISTS <-> (text, text);
DROP OPERATOR IF EXISTS <% (text, text);
DROP OPERATOR IF EXISTS <<-> (text, text);
DROP OPERATOR IF EXISTS <<% (text, text);
DROP OPERATOR IF EXISTS <<<-> (text, text);
