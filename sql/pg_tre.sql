-- Regression test for pg_tre extension

CREATE EXTENSION pg_tre;

-- Basic exact match (cost 0)
SELECT tre_amatch('hello', 'hello', 0);

-- Approximate match: 'colour' matches regex 'color' within distance 1
-- (one insertion: the 'u' in 'colour')
SELECT tre_amatch('colour', 'color', 1);

-- Regex approximate match: enviro.*ment matches 'environment' exactly
SELECT tre_amatch('environment', 'enviro.*ment', 2);

-- Cost function returns the actual edit cost
SELECT tre_amatch_cost('colour', 'color', 3);

-- No match within threshold
SELECT tre_amatch('abcdef', 'xyz', 1);

-- Custom per-operation costs (cost_ins=1, cost_del=1, cost_subst=1)
SELECT tre_amatch('colour', 'color', 2, 1, 1, 1);

-- Detail function shows cost breakdown
SELECT * FROM tre_amatch_detail('colour', 'color', 3);

-- Version string
SELECT tre_version();

-- Edge cases: empty strings
SELECT tre_amatch('', '', 0);
SELECT tre_amatch('a', '', 1);

-- NULL handling (STRICT functions return NULL on NULL input)
SELECT tre_amatch(NULL, 'test', 1);
SELECT tre_amatch('test', NULL, 1);
