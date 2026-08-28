#!/usr/bin/env python3
"""bench/stress/gen_large_corpus.py — streaming, seeded, scalable corpus.

Emits CSV (id,body) to stdout so it can pipe straight into psql \\copy
without staging the whole (possibly hundreds of GB) file twice:

    python3 gen_large_corpus.py --rows 10000000 --shape medium \\
      | psql -c "\\copy t(id,body) FROM STDIN WITH (FORMAT csv, HEADER true)"

Two independent axes drive the two pg_tre scaling costs:
  --rows    N            row count      -> tid_bloom RAM (~56 B/row)
  --shape   short|medium|long           mean row width -> build temp disk
                                          (~64 B per emitted trigram ~= /char)

Planted tokens at FIXED frequencies give deterministic, selectivity-
controlled query points at any scale, plus anchored variants (a fraction of
rows START with the token) so the SuRF ^-anchored path has real hits:

  common     5%    "government"     (and ~1% of rows start with it)
  mid        1%    "electrification"
  rare       0.1%  "naturalize"
  absent     0%    "zzqxby"         (never planted -> anchored-absent reject)

Deterministic: same --seed + args => identical corpus (so a re-run on a new
release compares like-for-like).
"""
import argparse, random, sys

ROOTS = ("system program network service config module handler request response "
         "session buffer kernel process thread socket packet router cache index "
         "query schema cluster replica backup monitor metric logger parser encoder "
         "decoder matrix vector gateway pipeline scheduler allocator compaction").split()
STOP = "the of and a to in is it for on as at by an be or with from into over".split()

SHAPES = {
    "short":  (6, 2),      # ~40-60 chars  (SKUs, log lines, identifiers)
    "medium": (20, 6),     # ~350-450 chars (subjects, short messages)
    "long":   (140, 40),   # ~2-4 KB       (email/document bodies)
}
PLANTED = {"government": 0.05, "electrification": 0.01, "naturalize": 0.001}
ANCHOR_TOKEN = "government"     # a fraction of rows START with this
ANCHOR_FRAC = 0.01

def typo(w, rnd):
    if len(w) < 4:
        return w
    p = rnd.randint(1, len(w) - 2)
    return w[:p] + rnd.choice("abcdefghijklmnopqrstuvwxyz") + w[p + 1:]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, required=True)
    ap.add_argument("--shape", choices=SHAPES, default="medium")
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--typo-rate", type=float, default=0.01,
                    help="per-word probability of a 1-char substitution (fuzzy realism)")
    ap.add_argument("--out", default="-", help="output file, or - for stdout")
    args = ap.parse_args()

    rnd = random.Random(args.seed)
    mean_words, sd_words = SHAPES[args.shape]
    # A modest vocabulary of numbered content words gives realistic distinct-
    # trigram growth that saturates (not linear in rows) — like real corpora.
    vocab_n = max(2000, min(200000, args.rows // 50))
    content = [f"{rnd.choice(ROOTS)}{rnd.randint(100, 99999)}" for _ in range(vocab_n)]

    f = sys.stdout if args.out == "-" else open(args.out, "w", buffering=1 << 20)
    try:
        f.write("id,body\n")
        planted_items = list(PLANTED.items())
        for i in range(1, args.rows + 1):
            k = max(3, int(rnd.gauss(mean_words, sd_words)))
            words = []
            for _ in range(k):
                words.append(rnd.choice(STOP) if rnd.random() < 0.5 else rnd.choice(content))
            for tok, p in planted_items:
                if rnd.random() < p:
                    words.insert(rnd.randint(0, len(words)), tok)
            if args.typo_rate > 0:
                words = [typo(w, rnd) if rnd.random() < args.typo_rate else w for w in words]
            if rnd.random() < ANCHOR_FRAC:
                words.insert(0, ANCHOR_TOKEN)   # anchored ^-hit
            body = " ".join(words).replace('"', "")
            f.write(f'{i},"{body}"\n')
            if args.out != "-" and (i % 1_000_000 == 0):
                sys.stderr.write(f"[gen] {i:,} rows\n")
    finally:
        if f is not sys.stdout:
            f.close()

if __name__ == "__main__":
    main()
