#!/usr/bin/env bash
# fetch-corpus.sh — Generate deterministic synthetic corpus for pg_tre benchmark
#
# Creates 100,000 rows of ~200-character text from a 10,000-word English vocabulary
# with intentional typos (~2%) for fuzzy-match realism.
#
# Output: bench/corpus.csv (id, body)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORPUS_FILE="${SCRIPT_DIR}/corpus.csv"
NUM_ROWS=1000
AVG_WORDS_PER_ROW=30
TYPO_RATE=0.02

# Deterministic seed for reproducibility
RANDOM_SEED=42

echo "==> Generating synthetic corpus..."
echo "    Rows: ${NUM_ROWS}"
echo "    Output: ${CORPUS_FILE}"

# Generate 10,000-word vocabulary (common English words)
# Using a deterministic list to ensure reproducibility
cat > "${SCRIPT_DIR}/vocab.txt" <<'EOF'
the and for are with that have this from they which will would their about been other what people some time into than more could these first after only world before over such between also many through just any those both because each most under should while where government work years system still used made between national public back same may following during without however used about does states general against used social country great local area being often made might large interest became members called based several less economic given since making American become human important form known development system until part case program significant within thought international period known various therefore become service industry whether believe consider whether major political complete community different following whether particular become possible activity certain individual special decision almost together production different standard military experience usually various important finally particular recent effect finally working various usually perhaps nothing beyond example important someone natural private various different significant required further particular outside actually despite sometimes natural evidence financial whether despite further actually significant majority million information
EOF

# If vocab is too short, duplicate it to reach 10k words
while [ "$(wc -l < "${SCRIPT_DIR}/vocab.txt")" -lt 10000 ]; do
    cat "${SCRIPT_DIR}/vocab.txt" "${SCRIPT_DIR}/vocab.txt" > "${SCRIPT_DIR}/vocab_tmp.txt"
    mv "${SCRIPT_DIR}/vocab_tmp.txt" "${SCRIPT_DIR}/vocab.txt"
done

# Truncate to exactly 10k unique words and shuffle deterministically
head -10000 "${SCRIPT_DIR}/vocab.txt" | sort | uniq > "${SCRIPT_DIR}/vocab_sorted.txt"
mv "${SCRIPT_DIR}/vocab_sorted.txt" "${SCRIPT_DIR}/vocab.txt"

echo "    Vocabulary: $(wc -l < "${SCRIPT_DIR}/vocab.txt") words"

# Generate corpus using Python for better random control
SCRIPT_DIR="${SCRIPT_DIR}" \
CORPUS_FILE="${CORPUS_FILE}" \
NUM_ROWS="${NUM_ROWS}" \
AVG_WORDS_PER_ROW="${AVG_WORDS_PER_ROW}" \
TYPO_RATE="${TYPO_RATE}" \
RANDOM_SEED="${RANDOM_SEED}" \
python3 <<'PYTHON_SCRIPT'
import random
import sys
import os

# Read config from environment
script_dir = os.environ['SCRIPT_DIR']
corpus_file = os.environ['CORPUS_FILE']
num_rows = int(os.environ['NUM_ROWS'])
avg_words = int(os.environ['AVG_WORDS_PER_ROW'])
typo_rate = float(os.environ['TYPO_RATE'])
seed = int(os.environ['RANDOM_SEED'])

# Set seed
random.seed(seed)

# Load vocabulary
with open(f'{script_dir}/vocab.txt', 'r') as f:
    vocab = [line.strip() for line in f if line.strip()]

# Helper: introduce typo (1 char substitution, insertion, or deletion)
def add_typo(word):
    if len(word) < 3:
        return word
    ops = ['sub', 'ins', 'del']
    op = random.choice(ops)
    pos = random.randint(1, len(word) - 2)  # avoid first/last char
    
    if op == 'sub':
        chars = 'abcdefghijklmnopqrstuvwxyz'
        new_char = random.choice(chars)
        return word[:pos] + new_char + word[pos+1:]
    elif op == 'ins':
        chars = 'abcdefghijklmnopqrstuvwxyz'
        new_char = random.choice(chars)
        return word[:pos] + new_char + word[pos:]
    else:  # del
        return word[:pos] + word[pos+1:]

# Generate corpus
print(f'Generating {num_rows} rows...', file=sys.stderr)
with open(corpus_file, 'w') as out:
    out.write('id,body\n')
    for row_id in range(1, num_rows + 1):
        # Random sentence length
        num_words = max(10, int(random.gauss(avg_words, avg_words / 3)))
        words = random.choices(vocab, k=num_words)
        
        # Introduce typos
        words = [add_typo(w) if random.random() < typo_rate else w for w in words]
        
        # Construct sentence
        body = ' '.join(words)
        
        # CSV escape (replace quotes)
        body = body.replace('"', '""')
        out.write(f'{row_id},"{body}"\n')
        
        if row_id % 10000 == 0:
            print(f'  {row_id}/{num_rows}...', file=sys.stderr)

print(f'Done. Wrote {num_rows} rows to {corpus_file}', file=sys.stderr)
PYTHON_SCRIPT

# Clean up vocab file
rm -f "${SCRIPT_DIR}/vocab.txt"

# Verify output
ACTUAL_ROWS=$(wc -l < "${CORPUS_FILE}")
EXPECTED_ROWS=$((NUM_ROWS + 1))  # +1 for header

if [ "${ACTUAL_ROWS}" -ne "${EXPECTED_ROWS}" ]; then
    echo "ERROR: Expected ${EXPECTED_ROWS} rows (including header), got ${ACTUAL_ROWS}"
    exit 1
fi

echo "==> Corpus generation complete"
echo "    File: ${CORPUS_FILE}"
echo "    Size: $(du -h "${CORPUS_FILE}" | cut -f1)"
