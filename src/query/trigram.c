/*
 * src/query/trigram.c - tokenizer.
 *
 * Phase 3: byte trigrams; Phase 3.5 replaces with codepoint trigrams.
 * The tokenizer emits (trigram_hash, position) pairs; position is the
 * byte offset in the original value, used later for positional
 * constraint application in the scan.
 */

#include "postgres.h"

#include "pg_tre/pg_tre.h"

/* Phase 3 bodies land here. */
