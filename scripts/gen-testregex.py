#!/usr/bin/env python3
"""Generate test/sql/testregex.sql from the AT&T regex(3) conformance corpus.

The corpus is the classic Glenn Fowler / AT&T Research test suite, mirrored at
codeberg/github gregburd/testregex as a set of ``*.dat`` files.  Each line is::

    flags<TAB>regex<TAB>input<TAB>expected[<TAB>...]

We turn it into a deterministic pg_regress test that exercises pg_tre's regex
engine -- in particular its *approximate* (edit-distance) matcher, which is the
whole reason the extension exists.  At least half of the emitted cases are
"approximate": a matching input is mutated by a few random edits (the
"approximate distance phrases") and must still match within that edit distance.

Construction rules:
  * Only ERE-flagged rows ('E'/'BE') are used; TRE compiles with REG_EXTENDED.
  * Rows using constructs TRE/Python disagree on (backreferences, collating
    elements, word boundaries, perl classes, lookarounds) are dropped, and the
    AT&T verdict is cross-checked against Python's ``re`` so every embedded case
    has an engine-agnostic, known-good verdict.
  * EXACT bucket (k=0): tre_amatch(input, regex, 0) must equal the verdict.
  * APPROX bucket (>=50% of cases): a positive row's input is mutated by
    d in {1,2} edits and tre_amatch(mutated, regex, k>=d) must still match.
    Anchored patterns (containing '^' or '$') are EXCLUDED from this bucket:
    inserting/deleting a character can make an empty-anchored match
    (e.g. '$^' on '') unmatchable at small k, so the naive edit-distance
    oracle is unsound for them.  They remain in the exact bucket.

Everything is driven by a fixed-seed RNG so the generated file is byte-stable.

Usage:
    python3 scripts/gen-testregex.py [CORPUS_DIR] [OUTPUT_SQL]

Defaults: CORPUS_DIR=~/src/testregex  OUTPUT_SQL=test/sql/testregex.sql
"""
import os
import re
import sys
import random

DAT_FILES = [
    "basic.dat",
    "categorize.dat",
    "forcedassoc.dat",
    "leftassoc.dat",
    "nullsubexpr.dat",
    "repetition.dat",
    "rightassoc.dat",
]

SEED = 0x7245_6745  # "TREG"
ALPHABET = "abcdefghijklmnopqrstuvwxyz"

# Constructs that TRE (POSIX ERE) and Python's re engine handle differently, or
# that TRE does not support; rows containing them in the pattern are dropped.
UNSUPPORTED = (
    re.compile(r"\\[0-9]"),     # backreferences
    re.compile(r"\[\[[.:=]"),   # collating elems / equivalence / char classes
    re.compile(r"\\[bBwWsSdD<>]"),  # word boundaries / perl classes
    re.compile(r"\(\?"),        # lookaround / non-capturing groups
)


def strip_meta(flags, field):
    """Strip testregex meta-prefixes from a regex/input field.

    Fields may be prefixed with the AT&T meta markers documented in the corpus
    README: a leading ':HA#NNN:' tag, or one of | ? { introducing alternate
    expectations.  We only need the plain ERE text.
    """
    # Drop a leading ':...:' annotation.
    if field.startswith(":"):
        end = field.find(":", 1)
        if end != -1:
            field = field[end + 1:]
    return field


def unescape_input(s):
    """Translate the corpus's literal-input conventions to a real string."""
    if s == "NULL":
        return ""
    # The corpus uses C-style escapes sparingly in inputs.
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
            mapping = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\"}
            if nxt in mapping:
                out.append(mapping[nxt])
                i += 2
                continue
        out.append(c)
        i += 1
    return "".join(out)


def parse_corpus(corpus_dir):
    """Return a list of (regex, input, positive) rows usable as ERE.

    positive is True if the AT&T verdict is a match-position tuple, False if
    NOMATCH.  Error-expectation rows (BADBR, etc.) are dropped.
    """
    rows = []
    for name in DAT_FILES:
        path = os.path.join(corpus_dir, name)
        if not os.path.exists(path):
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                line = raw.rstrip("\n")
                if not line or line.startswith("#") or line.startswith("NOTE"):
                    continue
                parts = line.split("\t")
                # Collapse runs of tabs: corpus uses tabs as padding.
                parts = [p for p in parts if p != ""]
                if len(parts) < 4:
                    continue
                flags, regex, inp, expected = parts[0], parts[1], parts[2], parts[3]
                # ERE only.
                if "E" not in flags:
                    continue
                regex = strip_meta(flags, regex)
                if not regex:
                    continue
                if any(p.search(regex) for p in UNSUPPORTED):
                    continue
                text = unescape_input(inp)

                exp = expected.strip()
                if exp.startswith("("):
                    positive = True
                elif exp == "NOMATCH":
                    positive = False
                else:
                    # error codes (BADBR, ECOLLATE, EXPECTED, ...) -> skip
                    continue

                # Cross-validate with Python's re engine; drop disagreements so
                # the oracle is engine-agnostic.
                try:
                    py_match = re.search(regex, text) is not None
                except re.error:
                    continue
                if py_match != positive:
                    continue

                rows.append((regex, text, positive))
    # Stable de-dup preserving order.
    seen = set()
    uniq = []
    for r in rows:
        if r in seen:
            continue
        seen.add(r)
        uniq.append(r)
    return uniq


def mutate(rng, s, d):
    """Apply exactly d random single-character edits to s and return result."""
    chars = list(s)
    for _ in range(d):
        op = rng.randint(0, 2)  # 0=insert 1=delete 2=substitute
        if not chars:
            op = 0  # only insertion is possible on an empty string
        if op == 0:
            pos = rng.randint(0, len(chars))
            chars.insert(pos, rng.choice(ALPHABET))
        elif op == 1:
            pos = rng.randint(0, len(chars) - 1)
            del chars[pos]
        else:
            pos = rng.randint(0, len(chars) - 1)
            chars[pos] = rng.choice(ALPHABET)
    return "".join(chars)


def is_anchored(regex):
    return "^" in regex or "$" in regex


def dollar_quote(s):
    """Quote a string for SQL using a $x$...$x$ tag that cannot collide.

    The corpus never contains the literal token ``$x$``; we assert that and
    fall back to escalating tags if it ever does.
    """
    tag = "x"
    while ("$" + tag + "$") in s:
        tag += "x"
    return "$" + tag + "$" + s + "$" + tag + "$"


def value_row(regex, inp, k, expect, kind):
    return "  (%s, %s, %d, %s, '%s')" % (
        dollar_quote(regex),
        dollar_quote(inp),
        k,
        "true" if expect else "false",
        kind,
    )


def build_cases(rows, rng):
    exact = []   # (regex, input, 0, positive, 'exact')
    for regex, inp, positive in rows:
        exact.append((regex, inp, 0, positive, "exact"))

    positives = [(r, i) for (r, i, p) in rows if p]
    # Approx bucket must exclude anchored patterns (see module docstring).
    approx_pool = [(r, i) for (r, i) in positives if not is_anchored(r)]

    n_exact = len(exact)
    # Emit exactly as many approx cases as exact ones -> 50.0% approx.
    approx = []
    j = 0
    for _ in range(n_exact):
        regex, inp = approx_pool[j % len(approx_pool)]
        j += 1
        d = rng.randint(1, 2)
        mutated = mutate(rng, inp, d)
        k = rng.randint(d, 2)  # k >= d so the mutated input still matches
        approx.append((regex, mutated, k, True, "approx"))

    return exact, approx


def render(exact, approx, npos, nneg):
    total = len(exact) + len(approx)
    pct = (len(approx) * 100.0) / total if total else 0.0
    lines = []
    A = lines.append
    A("-- ============================================================================")
    A("-- testregex.sql -- exercise pg_tre's regex engine with the AT&T regex(3)")
    A("-- conformance corpus (github/codeberg gregburd/testregex, the classic")
    A("-- Glenn Fowler / AT&T Research test suite).")
    A("--")
    A("-- This test is GENERATED by scripts/gen-testregex.py from the *.dat files")
    A("-- in that repository.  Do not edit by hand; re-run the generator to refresh.")
    A("--")
    A("-- Construction:")
    A("--   * Only ERE-flagged ('E'/'BE') rows are used (TRE compiles with")
    A("--     REG_EXTENDED).  Backreference / collating-element / word-boundary")
    A("--     rows and rows whose expectation diverges from a reference ERE engine")
    A("--     are dropped, so every embedded case has a known, engine-agnostic")
    A("--     verdict.")
    A("--   * EXACT bucket (k=0): tre_amatch(input, regex, 0) must equal the")
    A("--     corpus verdict (a position tuple => match; NOMATCH => no match).")
    A("--   * APPROX bucket (>=50% of all cases): the input of a matching row is")
    A("--     mutated by d in {1,2} random edits (insert / delete / substitute --")
    A("--     the \"approximate distance phrases\"), then tre_amatch(mutated, regex,")
    A("--     k) with k>=d must still match.  Anchored (^/$) patterns are excluded")
    A("--     from this bucket; they remain in the exact bucket.  Mutations come")
    A("--     from a fixed-seed RNG so this file is fully deterministic.")
    A("--")
    A("-- Totals baked into this run:")
    A("--   cases=%d  exact=%d  approx=%d  (%.1f%% approx)" % (total, len(exact), len(approx), pct))
    A("--   positive=%d  negative=%d" % (npos, nneg))
    A("-- ============================================================================")
    A("")
    A("CREATE EXTENSION IF NOT EXISTS pg_tre;")
    A("")
    A("SET client_min_messages = 'warning';")
    A("")
    A("-- The generated corpus: (regex, input, k, expect, kind).")
    A("CREATE TEMP TABLE tr_corpus(regex text, input text, k int, expect bool, kind text);")
    A("INSERT INTO tr_corpus(regex, input, k, expect, kind) VALUES")
    all_rows = exact + approx
    for idx, (regex, inp, k, expect, kind) in enumerate(all_rows):
        sep = "," if idx < len(all_rows) - 1 else ";"
        lines.append(value_row(regex, inp, k, expect, kind) + sep)
    A("")
    A("-- ----------------------------------------------------------------------------")
    A("-- 1. Functional oracle: every case's tre_amatch verdict must equal expect.")
    A("--    Report only aggregate pass/fail counts (deterministic), plus the first")
    A("--    few mismatches if any, so a regression is actionable without dumping")
    A("--    600+ rows.")
    A("-- ----------------------------------------------------------------------------")
    A("SELECT kind,")
    A("       count(*)                                   AS cases,")
    A("       count(*) FILTER (WHERE tre_amatch(input, regex, k) = expect) AS passed,")
    A("       count(*) FILTER (WHERE tre_amatch(input, regex, k) <> expect) AS failed")
    A("FROM tr_corpus")
    A("GROUP BY kind")
    A("ORDER BY kind;")
    A("")
    A("-- Any individual mismatches (expected to be empty).")
    A("SELECT regex, input, k, expect, tre_amatch(input, regex, k) AS got")
    A("FROM tr_corpus")
    A("WHERE tre_amatch(input, regex, k) <> expect")
    A("ORDER BY regex, input, k;")
    A("")
    A("-- ----------------------------------------------------------------------------")
    A("-- 2. Coverage guarantee: at least half of all cases are approximate")
    A("--    (exercise the edit-distance matcher, pg_tre's reason for existing).")
    A("-- ----------------------------------------------------------------------------")
    A("SELECT (count(*) FILTER (WHERE kind = 'approx') * 100 / count(*)) >= 50")
    A("         AS at_least_half_approx,")
    A("       count(*) FILTER (WHERE kind = 'approx') AS approx_cases,")
    A("       count(*)                                AS total_cases")
    A("FROM tr_corpus;")
    A("")
    A("-- ----------------------------------------------------------------------------")
    A("-- 3. Index path agreement: for a deterministic sample, the indexed")
    A("--    operator (text %~~ tre_pattern(regex, k)) must return exactly the")
    A("--    same row set as the sequential tre_amatch() scan.")
    A("-- ----------------------------------------------------------------------------")
    A("CREATE TEMP TABLE tr_docs(id serial PRIMARY KEY, body text);")
    A("INSERT INTO tr_docs(body)")
    A("  SELECT DISTINCT input FROM tr_corpus WHERE input <> '' ORDER BY 1;")
    A("CREATE INDEX tr_docs_idx ON tr_docs USING tre (body);")
    A("")
    A("-- Compare seq vs index for a spread of (regex, k) probes drawn from the")
    A("-- corpus.  Returns one 'OK'/'BAD' row per probe.")
    A("CREATE OR REPLACE FUNCTION tr_probe(pat text, kk int) RETURNS text")
    A("LANGUAGE plpgsql AS $$")
    A("DECLARE seq_ids int[]; idx_ids int[];")
    A("BEGIN")
    A("  SET LOCAL enable_indexscan=off; SET LOCAL enable_bitmapscan=off; SET LOCAL enable_seqscan=on;")
    A("  EXECUTE format('SELECT array_agg(id ORDER BY id) FROM tr_docs WHERE tre_amatch(body, %L, %s)', pat, kk)")
    A("    INTO seq_ids;")
    A("  SET LOCAL enable_seqscan=off; SET LOCAL enable_indexscan=on; SET LOCAL enable_bitmapscan=on;")
    A("  EXECUTE format('SELECT array_agg(id ORDER BY id) FROM tr_docs WHERE body %%~~ tre_pattern(%L, %s)', pat, kk)")
    A("    INTO idx_ids;")
    A("  IF seq_ids IS NOT DISTINCT FROM idx_ids THEN RETURN 'OK   k='||kk||'  '||pat; END IF;")
    A("  RETURN format('BAD k=%s %s  seq=%s  idx=%s', kk, pat, seq_ids, idx_ids);")
    A("END$$;")
    A("")
    A("SELECT tr_probe('a.c', 0);")
    A("SELECT tr_probe('a.c', 1);")
    A("SELECT tr_probe('(ab|cd)', 0);")
    A("SELECT tr_probe('(ab|cd)', 1);")
    A("SELECT tr_probe('a[b-d]e', 0);")
    A("SELECT tr_probe('a[b-d]e', 2);")
    A("SELECT tr_probe('xyz', 0);")
    A("SELECT tr_probe('xyz', 1);")
    A("")
    A("RESET client_min_messages;")
    A("")
    A("DROP FUNCTION tr_probe(text, int);")
    A("DROP INDEX tr_docs_idx;")
    A("DROP TABLE tr_docs;")
    A("DROP TABLE tr_corpus;")
    A("")
    return "\n".join(lines)


def main(argv):
    home = os.path.expanduser("~")
    corpus_dir = argv[1] if len(argv) > 1 else os.path.join(home, "src", "testregex")
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    out_path = argv[2] if len(argv) > 2 else os.path.join(repo, "test", "sql", "testregex.sql")

    rows = parse_corpus(corpus_dir)
    if not rows:
        sys.stderr.write("error: no usable rows parsed from %s\n" % corpus_dir)
        return 1
    npos = sum(1 for r in rows if r[2])
    nneg = sum(1 for r in rows if not r[2])

    rng = random.Random(SEED)
    exact, approx = build_cases(rows, rng)
    sql = render(exact, approx, npos, nneg)

    with open(out_path, "w", encoding="utf-8") as fh:
        fh.write(sql)
    sys.stderr.write(
        "wrote %s: cases=%d exact=%d approx=%d (%.1f%% approx) positive=%d negative=%d\n"
        % (out_path, len(exact) + len(approx), len(exact), len(approx),
           (len(approx) * 100.0) / (len(exact) + len(approx)), npos, nneg)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
