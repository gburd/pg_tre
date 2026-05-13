#!/bin/bash
# fuzz/create_corpus.sh - Generate seed corpus for pg_tre regex parser

CORPUS_DIR="corpus"

# Simple patterns
echo -n "a" > "${CORPUS_DIR}/01_literal"
echo -n "abc" > "${CORPUS_DIR}/02_word"
echo -n "hello world" > "${CORPUS_DIR}/03_phrase"

# Operators
echo -n "a*" > "${CORPUS_DIR}/04_star"
echo -n "a+" > "${CORPUS_DIR}/05_plus"
echo -n "a?" > "${CORPUS_DIR}/06_optional"
echo -n "a|b" > "${CORPUS_DIR}/07_alt"
echo -n "(abc)" > "${CORPUS_DIR}/08_group"

# Repetition
echo -n "a{3}" > "${CORPUS_DIR}/09_exact"
echo -n "a{2,5}" > "${CORPUS_DIR}/10_range"
echo -n "a{1,}" > "${CORPUS_DIR}/11_unbounded"

# Character classes
echo -n "." > "${CORPUS_DIR}/12_any"
echo -n "[abc]" > "${CORPUS_DIR}/13_class"
echo -n "[a-z]" > "${CORPUS_DIR}/14_class_range"
echo -n "[^0-9]" > "${CORPUS_DIR}/15_class_neg"

# Anchors
echo -n "^start" > "${CORPUS_DIR}/16_anchor_start"
echo -n "end$" > "${CORPUS_DIR}/17_anchor_end"

# Approximate
echo -n "hello{~1}" > "${CORPUS_DIR}/18_approx_k1"
echo -n "world{~2}" > "${CORPUS_DIR}/19_approx_k2"

# Complex
echo -n "(a|b)c" > "${CORPUS_DIR}/20_complex1"
echo -n "a(b|c)" > "${CORPUS_DIR}/21_complex2"
echo -n "(a*){~1}" > "${CORPUS_DIR}/22_approx_rep"

# Edge cases
echo -n "" > "${CORPUS_DIR}/23_empty"
echo -n "[" > "${CORPUS_DIR}/24_unclosed_class"
echo -n "(" > "${CORPUS_DIR}/25_unclosed_paren"
echo -n "*" > "${CORPUS_DIR}/26_dangling_star"

echo "Created ${CORPUS_DIR} with $(ls ${CORPUS_DIR} | wc -l) files"
