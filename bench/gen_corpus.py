#!/usr/bin/env python3
import random, sys

random.seed(42)
N_ROWS = int(sys.argv[1]) if len(sys.argv) > 1 else 1000000
OUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/corpus_1m.csv"

# Load dictionary
words = []
with open("/usr/share/dict/words") as f:
    for line in f:
        w = line.strip()
        if w and len(w) <= 20 and w.isalpha():
            words.append(w)
print(f"loaded {len(words)} words", file=sys.stderr)

with open(OUT, "w") as f:
    for i in range(1, N_ROWS + 1):
        prefix = ""
        if i % 100 == 0:
            prefix = "connection refused after timeout "
        elif i % 1000 == 0:
            prefix = f"error E-{(i % 9999):04d} "
        if i % 20 == 0:
            prefix = prefix + "database "
        n_words = 5 + (i % 8)
        body = prefix + " ".join(random.choice(words) for _ in range(n_words))
        # CSV: id,body — escape body's commas/quotes by quoting
        # body has only letters and spaces from dict, so no escaping needed for our case
        f.write(f"{i},{body}\n")
print(f"wrote {N_ROWS} rows to {OUT}", file=sys.stderr)
