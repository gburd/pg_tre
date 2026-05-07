/*-------------------------------------------------------------------------
 *
 * chunk_codec.h
 *	  Sparsemap chunk-level codec: types, constants, and operations.
 *
 * This header extracts the pure chunk-level operations from sparsemap.c
 * so they can be shared with the hybrid bitmapset implementation.
 *
 * All functions are static (inline where appropriate) and operate on
 * raw chunk data (__sm_chunk_t) without any sparsemap_t dependency.
 *
 * Copyright (c) 2024 Gregory Burd <greg@burd.me>.  All rights reserved.
 * MIT License (see sparsemap.c for full text).
 *
 *-------------------------------------------------------------------------
 */
#ifndef CHUNK_CODEC_H
#define CHUNK_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include "popcount.h"

/*
 * Core types
 */
typedef uint64_t __sm_bitvec_t;
typedef uint64_t __sm_idx_t;

typedef struct {
  __sm_bitvec_t *m_data;
} __sm_chunk_t;

typedef struct {
  size_t rem;
  size_t pos;
} __sm_chunk_rank_t;

/*
 * Diagnostic macros (no-ops when not in sparsemap.c)
 */
#ifndef __sm_assert
#define __sm_assert(expr) ((void)0)
#endif

/*
 * Constants
 */
enum __SM_CHUNK_INFO {
  /* metadata overhead: sizeof(__sm_idx_t) bytes for chunk count */
  SM_SIZEOF_OVERHEAD = sizeof(__sm_idx_t),

  /* number of bits that can be stored in a __sm_bitvec_t */
  SM_BITS_PER_VECTOR = sizeof(__sm_bitvec_t) * 8,

  /* number of flags that can be stored in a single index byte */
  SM_FLAGS_PER_INDEX_BYTE = 4,

  /* number of flags that can be stored in the index */
  SM_FLAGS_PER_INDEX = sizeof(__sm_bitvec_t) * SM_FLAGS_PER_INDEX_BYTE,

  /* maximum capacity of a __sm_chunk_t (in bits) */
  SM_CHUNK_MAX_CAPACITY = SM_BITS_PER_VECTOR * SM_FLAGS_PER_INDEX,

  /* maximum capacity of a __sm_chunk_t (31 bits of the RLE) */
  SM_CHUNK_RLE_MAX_CAPACITY = 0x7FFFFFFF,

  /* minimum capacity of a __sm_chunk_t (in bits) */
  SM_CHUNK_MIN_CAPACITY = SM_BITS_PER_VECTOR - 2,

  /* maximum length of a __sm_chunk_t (31 bits of the RLE) */
  SM_CHUNK_RLE_MAX_LENGTH = 0x7FFFFFFF,

  /* __sm_bitvec_t payload is all zeros (2#00) */
  SM_PAYLOAD_ZEROS = 0,

  /* __sm_bitvec_t payload is all ones (2#11) */
  SM_PAYLOAD_ONES = 3,

  /* __sm_bitvec_t payload is mixed (2#10) */
  SM_PAYLOAD_MIXED = 2,

  /* __sm_bitvec_t is not used (2#01) */
  SM_PAYLOAD_NONE = 1,

  /* a mask for checking flags (2 bits, 2#11) */
  SM_FLAG_MASK = 3,

  /* return code for set(): ok, no further action required */
  SM_OK = 0,

  /* return code for set(): needs to grow this __sm_chunk_t */
  SM_NEEDS_TO_GROW = 1,

  /* return code for set(): needs to shrink this __sm_chunk_t */
  SM_NEEDS_TO_SHRINK = 2
};

/*
 * Flag manipulation macros
 */
#define SM_CHUNK_GET_FLAGS(data, at) ((((data)) & ((__sm_bitvec_t)SM_FLAG_MASK << ((at)*2))) >> ((at)*2))
#define SM_CHUNK_SET_FLAGS(data, at, to) ((data) = ((data) & ~((__sm_bitvec_t)SM_FLAG_MASK << ((at)*2))) | ((__sm_bitvec_t)(to) << ((at)*2)))
#define SM_IS_CHUNK_RLE(chunk) \
  (((*((__sm_bitvec_t *)(chunk)->m_data) & (((__sm_bitvec_t)0x3) << (SM_BITS_PER_VECTOR - 2))) >> (SM_BITS_PER_VECTOR - 2)) == SM_PAYLOAD_NONE)

/*
 * RLE (Run-Length Encoding) Format
 *
 * Bits 63:62 = 01 (RLE flag, matches SM_PAYLOAD_NONE to distinguish from sparse)
 * Bits 61:31 = Chunk capacity in bits (31 bits, max 2,147,483,647)
 * Bits 30:0  = Run length in bits (31 bits, max 2,147,483,647)
 */
#define SM_RLE_FLAGS 0x4000000000000000          /* Bits 63:62 = 01 */
#define SM_RLE_FLAGS_MASK 0xC000000000000000     /* Mask for bits 63:62 */
#define SM_RLE_CAPACITY_MASK 0x3FFFFFFF80000000  /* Mask for bits 61:31 (capacity) */
#define SM_RLE_LENGTH_MASK 0x7FFFFFFF            /* Mask for bits 30:0 (length) */

/* ----------------------------------------------------------------
 * RLE detection and manipulation
 * ---------------------------------------------------------------- */

static inline __attribute__((always_inline)) bool
__sm_chunk_is_rle(const __sm_chunk_t *chunk)
{
  const __sm_bitvec_t w = chunk->m_data[0];
  return (w & SM_RLE_FLAGS_MASK) == SM_RLE_FLAGS;
}

static inline void
__sm_chunk_set_rle(const __sm_chunk_t *chunk)
{
  __sm_bitvec_t w = chunk->m_data[0];
  w &= ~(SM_RLE_FLAGS_MASK | SM_RLE_CAPACITY_MASK | SM_RLE_LENGTH_MASK);
  w |= ((((__sm_bitvec_t)1) << (SM_BITS_PER_VECTOR - 2)) & SM_RLE_FLAGS_MASK);
  chunk->m_data[0] = w;
}

static inline size_t
__sm_chunk_rle_get_capacity(const __sm_chunk_t *chunk)
{
  __sm_bitvec_t w = chunk->m_data[0] & (__sm_bitvec_t)SM_RLE_CAPACITY_MASK;
  w >>= 31;
  return w;
}

static inline void
__sm_chunk_rle_set_capacity(const __sm_chunk_t *chunk, const size_t capacity)
{
  __sm_assert(capacity <= SM_CHUNK_RLE_MAX_CAPACITY);
  __sm_bitvec_t w = chunk->m_data[0];
  w &= ~SM_RLE_CAPACITY_MASK;
  w |= (capacity << 31) & SM_RLE_CAPACITY_MASK;
  chunk->m_data[0] = w;
}

static inline size_t
__sm_chunk_rle_get_length(const __sm_chunk_t *chunk)
{
  const __sm_bitvec_t w = chunk->m_data[0] & (__sm_bitvec_t)SM_RLE_LENGTH_MASK;
  return w;
}

static inline void
__sm_chunk_rle_set_length(const __sm_chunk_t *chunk, const size_t length)
{
  __sm_assert(length <= SM_CHUNK_RLE_MAX_LENGTH);
  __sm_assert(length <= __sm_chunk_rle_get_capacity(chunk));
  __sm_bitvec_t w = chunk->m_data[0];
  w &= ~SM_RLE_LENGTH_MASK;
  w |= length & SM_RLE_LENGTH_MASK;
  chunk->m_data[0] = w;
}

/* ----------------------------------------------------------------
 * Run length (works for both RLE and sparse chunks)
 * ---------------------------------------------------------------- */

static inline size_t
__sm_chunk_get_run_length(const __sm_chunk_t *chunk)
{
  size_t length = 0;

  if (__sm_chunk_is_rle(chunk)) {
    length = __sm_chunk_rle_get_length(chunk);
  } else {
    size_t count = 0;
    int j = SM_FLAGS_PER_INDEX, k = SM_BITS_PER_VECTOR;
    __sm_bitvec_t w = chunk->m_data[0], v = chunk->m_data[1];

    switch (w) {
    case 0:
      return 0;
    case ~(__sm_bitvec_t)0:
      return SM_CHUNK_MAX_CAPACITY;
    default:
      while (j && (w & SM_PAYLOAD_ONES) == SM_PAYLOAD_ONES) {
        count++;
        w >>= 2;
        j--;
      }
      if (count) {
        count *= SM_BITS_PER_VECTOR;
        if ((w & SM_PAYLOAD_MIXED) == SM_PAYLOAD_MIXED) {
          w >>= 2;
          j--;
          while (k && (v & 1) == 1) {
            count++;
            v >>= 1;
            k--;
          }
          while (k && (v & 1) == 0) {
            v >>= 1;
            k--;
          }
          if (k) {
            return 0;
          }
        }
        while (j--) {
          switch (w & 0x3) {
          case SM_PAYLOAD_NONE:
          case SM_PAYLOAD_ZEROS:
            w >>= 2;
            break;
          default:
            return 0;
          }
        }
        __sm_assert(count < SM_CHUNK_MAX_CAPACITY);
        length = count;
      }
    }
  }
  return length;
}

/* ----------------------------------------------------------------
 * Chunk size and capacity
 * ---------------------------------------------------------------- */

static inline size_t
__sm_chunk_calc_vector_size(const uint8_t b)
{
  // clang-format off
  static int lookup[] = {
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    2,  2,  3,  2,  2,  2,  3,  2,  3,  3,  4,  3,  2,  2,  3,  2,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0
  };
  // clang-format on
  return lookup[b];
}

static inline __attribute__((always_inline)) size_t
__sm_chunk_get_position(const __sm_chunk_t *chunk, size_t bv)
{
  size_t position = 0;
  register uint8_t *p = (uint8_t *)chunk->m_data;

  if (!__sm_chunk_is_rle(chunk)) {
    const size_t num_bytes = bv / ((size_t)SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR);
    for (size_t i = 0; i < num_bytes; i++, p++) {
      position += __sm_chunk_calc_vector_size(*p);
    }

    bv -= num_bytes * SM_FLAGS_PER_INDEX_BYTE;
    for (size_t i = 0; i < bv; i++) {
      const size_t flags = SM_CHUNK_GET_FLAGS(*chunk->m_data, i);
      if (flags == SM_PAYLOAD_MIXED) {
        position++;
      }
    }
  }

  return position;
}

static inline void
__sm_chunk_init(__sm_chunk_t *chunk, uint8_t *data)
{
  chunk->m_data = (__sm_bitvec_t *)data;
}

static inline __attribute__((always_inline)) size_t
__sm_chunk_get_capacity(const __sm_chunk_t *chunk)
{
  if (__builtin_expect(__sm_chunk_is_rle(chunk), 0)) {
    return __sm_chunk_rle_get_capacity(chunk);
  }

  size_t capacity = SM_CHUNK_MAX_CAPACITY;
  register uint8_t *p = (uint8_t *)chunk->m_data;

  for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
    if (!*p || *p == 0xff) {
      continue;
    }
    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      const size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        capacity -= SM_BITS_PER_VECTOR;
      }
    }
  }
  return capacity;
}

static inline void
__sm_chunk_increase_capacity(const __sm_chunk_t *chunk, const size_t capacity)
{
  __sm_assert(capacity % SM_BITS_PER_VECTOR == 0);
  __sm_assert(capacity <= SM_CHUNK_MAX_CAPACITY);
  __sm_assert(capacity > __sm_chunk_get_capacity(chunk));

  const size_t initial_capacity = __sm_chunk_get_capacity(chunk);
  if (capacity <= initial_capacity || capacity > SM_CHUNK_MAX_CAPACITY) {
    return;
  }

  size_t increased = 0;
  register uint8_t *p = (uint8_t *)chunk->m_data;
  for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
    if (!*p || *p == 0xff) {
      continue;
    }
    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      const size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        *p &= ~((__sm_bitvec_t)SM_PAYLOAD_ONES << j * 2);
        *p |= (__sm_bitvec_t)SM_PAYLOAD_ZEROS << j * 2;
        increased += SM_BITS_PER_VECTOR;
        if (increased + initial_capacity == capacity) {
          __sm_assert(__sm_chunk_get_capacity(chunk) == capacity);
          return;
        }
      }
    }
  }
  __sm_assert(__sm_chunk_get_capacity(chunk) == capacity);
}

static inline bool
__sm_chunk_is_empty(const __sm_chunk_t *chunk)
{
  if (chunk->m_data[0] != 0) {
    register uint8_t *p = (uint8_t *)chunk->m_data;
    for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
      if (*p) {
        for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
          const size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
          if (flags != SM_PAYLOAD_NONE && flags != SM_PAYLOAD_ZEROS) {
            return false;
          }
        }
      }
    }
  }
  return true;
}

static inline __attribute__((always_inline)) size_t
__sm_chunk_get_size(const __sm_chunk_t *chunk)
{
  size_t size = sizeof(__sm_bitvec_t);
  if (__builtin_expect(!__sm_chunk_is_rle(chunk), 1)) {
    register uint8_t *p = (uint8_t *)chunk->m_data;
    for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
      size += sizeof(__sm_bitvec_t) * __sm_chunk_calc_vector_size(*p);
    }
  }
  return size;
}

/* ----------------------------------------------------------------
 * Bit testing and manipulation
 * ---------------------------------------------------------------- */

static inline __attribute__((always_inline)) bool
__sm_chunk_is_set(const __sm_chunk_t *chunk, const size_t idx)
{
  if (__builtin_expect(__sm_chunk_is_rle(chunk), 0)) {
    if (idx < __sm_chunk_rle_get_length(chunk)) {
      return true;
    }
    return false;
  }
  const size_t bv = idx / SM_BITS_PER_VECTOR;
  __sm_assert(bv < SM_FLAGS_PER_INDEX);

  const size_t flags = SM_CHUNK_GET_FLAGS(*chunk->m_data, bv);
  switch (flags) {
  case SM_PAYLOAD_ZEROS:
  case SM_PAYLOAD_NONE:
    return false;
  case SM_PAYLOAD_ONES:
    return true;
  default:
    __sm_assert(flags == SM_PAYLOAD_MIXED);
    /* FALLTHROUGH */
  }

  const __sm_bitvec_t w = chunk->m_data[1 + __sm_chunk_get_position(chunk, bv)];
  return (w & (__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR) > 0;
}

static inline int
__sm_chunk_clr_bit(const __sm_chunk_t *chunk, const uint64_t idx, size_t *pos)
{
  __sm_bitvec_t w;
  const size_t bv = idx / SM_BITS_PER_VECTOR;

  __sm_assert(bv < SM_FLAGS_PER_INDEX);

  switch (SM_CHUNK_GET_FLAGS(*chunk->m_data, bv)) {
  case SM_PAYLOAD_ZEROS:
    *pos = 0;
    return SM_OK;
    break;
  case SM_PAYLOAD_ONES:
    if (*pos == 0) {
      *pos = (size_t)1 + __sm_chunk_get_position(chunk, bv);
      return SM_NEEDS_TO_GROW;
    }
    SM_CHUNK_SET_FLAGS(*chunk->m_data, bv, SM_PAYLOAD_MIXED);
    w = chunk->m_data[*pos];
    w &= ~((__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR);
    chunk->m_data[*pos] = w;
    return SM_OK;
    break;
  case SM_PAYLOAD_MIXED:
    *pos = 1 + __sm_chunk_get_position(chunk, bv);
    w = chunk->m_data[*pos];
    w &= ~((__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR);
    if (w == 0) {
      SM_CHUNK_SET_FLAGS(*chunk->m_data, bv, SM_PAYLOAD_ZEROS);
      return SM_NEEDS_TO_SHRINK;
    }
    chunk->m_data[*pos] = w;
    break;
  case SM_PAYLOAD_NONE:
    /* FALLTHROUGH */
  default:
    __sm_assert(!"shouldn't be here");
#ifdef DEBUG
    abort();
#endif
    break;
  }
  return SM_OK;
}

static inline int
__sm_chunk_set_bit(const __sm_chunk_t *chunk, const uint64_t idx, size_t *pos)
{
  const size_t bv = idx / SM_BITS_PER_VECTOR;
  __sm_assert(bv < SM_FLAGS_PER_INDEX);
  __sm_assert(__sm_chunk_is_rle(chunk) == false);

  switch (SM_CHUNK_GET_FLAGS(*chunk->m_data, bv)) {
  case SM_PAYLOAD_ONES:
    *pos = 0;
    return SM_OK;
    break;
  case SM_PAYLOAD_ZEROS:
    if (*pos == 0) {
      *pos = (size_t)1 + __sm_chunk_get_position(chunk, bv);
      return SM_NEEDS_TO_GROW;
    }
    SM_CHUNK_SET_FLAGS(*chunk->m_data, bv, SM_PAYLOAD_MIXED);
    /* FALLTHROUGH */
  case SM_PAYLOAD_MIXED:
    *pos = 1 + __sm_chunk_get_position(chunk, bv);
    __sm_bitvec_t w = chunk->m_data[*pos];
    w |= (__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR;
    if (w == ~(__sm_bitvec_t)0) {
      SM_CHUNK_SET_FLAGS(*chunk->m_data, bv, SM_PAYLOAD_ONES);
      return SM_NEEDS_TO_SHRINK;
    }
    chunk->m_data[*pos] = w;
    break;
  case SM_PAYLOAD_NONE:
    /* FALLTHROUGH */
  default:
#ifdef DEBUG
    abort();
#endif
    break;
  }
  return SM_OK;
}

/* ----------------------------------------------------------------
 * Chunk select, rank, and scan
 * ---------------------------------------------------------------- */

static inline size_t
__sm_chunk_select(const __sm_chunk_t *chunk, ssize_t n, ssize_t *offset, const bool value)
{
  /* RLE fast path */
  if (__builtin_expect(__sm_chunk_is_rle(chunk), 0)) {
    const size_t length = __sm_chunk_rle_get_length(chunk);
    const size_t capacity = __sm_chunk_rle_get_capacity(chunk);

    if (value) {
      if (n < (ssize_t)length) {
        *offset = -1;
        return n;
      } else {
        *offset = n - length;
        return capacity;
      }
    } else {
      if (length >= capacity) {
        *offset = n;
        return capacity;
      }
      const size_t unset_count = capacity - length;
      if (n < (ssize_t)unset_count) {
        *offset = -1;
        return length + n;
      } else {
        *offset = n - unset_count;
        return capacity;
      }
    }
  }

  size_t ret = 0;
  register uint8_t *p = (uint8_t *)chunk->m_data;
  for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
    if (*p == 0 && value) {
      ret += (size_t)SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR;
      continue;
    }

    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      const size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        continue;
      }
      if (flags == SM_PAYLOAD_ZEROS) {
        if (value == true) {
          ret += SM_BITS_PER_VECTOR;
          continue;
        }
        if (n > SM_BITS_PER_VECTOR) {
          n -= SM_BITS_PER_VECTOR;
          ret += SM_BITS_PER_VECTOR;
          continue;
        }
        *offset = -1;
        return ret + n;
      }
      if (flags == SM_PAYLOAD_ONES) {
        if (value == true) {
          if (n > SM_BITS_PER_VECTOR) {
            n -= SM_BITS_PER_VECTOR;
            ret += SM_BITS_PER_VECTOR;
            continue;
          }
          *offset = -1;
          return ret + n;
        }
        ret += SM_BITS_PER_VECTOR;
        continue;
      }
      if (flags == SM_PAYLOAD_MIXED) {
        const __sm_bitvec_t w = chunk->m_data[1 + __sm_chunk_get_position(chunk, (i * SM_FLAGS_PER_INDEX_BYTE) + j)];
        __sm_bitvec_t target_bits = value ? w : ~w;
        __sm_bitvec_t remaining = target_bits;
        while (remaining) {
          int k = __builtin_ctzll(remaining);
          if (n == 0) {
            *offset = -1;
            return ret + (size_t)k;
          }
          n--;
          remaining &= remaining - 1;
        }
        ret += SM_BITS_PER_VECTOR;
      }
    }
  }
  *offset = n;
  return ret;
}

static inline size_t
__sm_chunk_rank(__sm_chunk_rank_t *rank, const bool value, const __sm_chunk_t *chunk, size_t from, size_t to)
{
  size_t amt = 0;
  const size_t cap = __sm_chunk_get_capacity(chunk);

  __sm_assert(to >= from);
  rank->rem = cap;
  rank->pos = 0;

  if (from >= cap) {
    rank->pos = cap;
    rank->rem = 0;
    return amt;
  }

  if (__builtin_expect(SM_IS_CHUNK_RLE(chunk), 0)) {
    const size_t length = __sm_chunk_rle_get_length(chunk);
    const size_t end = length - 1;
    if (to >= cap) {
      to = cap - 1;
    }
    rank->rem = 0;
    if (value) {
      if (from <= end) {
        amt = (to > end ? end : to) - from + 1;
        rank->pos = to + 1;
      } else {
        rank->pos = cap;
      }
    } else {
      if (from > end) {
        amt = to - from + 1;
        rank->pos = to + 1;
      } else if (to > end) {
        amt = to - end;
        rank->pos = to + 1;
      } else {
        rank->pos = to + 1;
      }
    }
  } else {
    uint8_t *vec = (uint8_t *)chunk->m_data;
    __sm_bitvec_t w, mw;
    uint64_t mask;
    size_t pc;

    for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, vec++) {
      for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
        const size_t flags = SM_CHUNK_GET_FLAGS(*vec, j);

        switch (flags) {

        case SM_PAYLOAD_ZEROS:
          rank->rem = 0;
          if (to >= SM_BITS_PER_VECTOR) {
            rank->pos += SM_BITS_PER_VECTOR;
            to -= SM_BITS_PER_VECTOR;
            if (from >= SM_BITS_PER_VECTOR) {
              from = from - SM_BITS_PER_VECTOR;
            } else {
              if (!value) {
                amt += SM_BITS_PER_VECTOR - from;
              }
              from = 0;
            }
          } else {
            rank->pos += to + 1;
            if (!value) {
              if (from > to) {
                from -= to;
              } else {
                amt += to + 1 - from;
                goto chunk_rank_done;
              }
            } else {
              goto chunk_rank_done;
            }
          }
          break;

        case SM_PAYLOAD_ONES:
          rank->rem = UINT64_MAX;
          if (to >= SM_BITS_PER_VECTOR) {
            rank->pos += SM_BITS_PER_VECTOR;
            to -= SM_BITS_PER_VECTOR;
            if (from >= SM_BITS_PER_VECTOR) {
              from = from - SM_BITS_PER_VECTOR;
            } else {
              if (value) {
                amt += SM_BITS_PER_VECTOR - from;
              }
              from = 0;
            }
          } else {
            rank->pos += to + 1;
            if (value) {
              if (from > to) {
                from = from - to;
              } else {
                amt += to + 1 - from;
                goto chunk_rank_done;
              }
            } else {
              goto chunk_rank_done;
            }
          }
          break;

        case SM_PAYLOAD_MIXED:
          w = chunk->m_data[1 + __sm_chunk_get_position(chunk, (i * SM_FLAGS_PER_INDEX_BYTE) + j)];
          if (to >= SM_BITS_PER_VECTOR) {
            rank->pos += SM_BITS_PER_VECTOR;
            to -= SM_BITS_PER_VECTOR;
            mask = from == 0 ? UINT64_MAX : ~(UINT64_MAX >> (SM_BITS_PER_VECTOR - (from >= 64 ? 64 : from)));
            mw = (value ? w : ~w) & mask;
            pc = popcountll(mw);
            amt += pc;
            from = from > SM_BITS_PER_VECTOR ? from - SM_BITS_PER_VECTOR : 0;
          } else {
            rank->pos += to + 1;
            const uint64_t to_mask = (to == 63) ? UINT64_MAX : ((uint64_t)1 << (to + 1)) - 1;
            const uint64_t from_mask = from == 0 ? UINT64_MAX : ~(UINT64_MAX >> (SM_BITS_PER_VECTOR - (from >= 64 ? 64 : from)));
            mask = to_mask & from_mask;
            mw = (value ? w : ~w) & mask;
            pc = popcountll(mw);
            amt += pc;
            rank->rem = mw >> (from > 63 ? 63 : from);
            goto chunk_rank_done;
          }
          break;

        case SM_PAYLOAD_NONE:
        default:
          continue;
        }
      }
    }
  }
chunk_rank_done:;
  return amt;
}

static inline size_t
__sm_chunk_scan(const __sm_chunk_t *chunk, const __sm_idx_t start, void (*scanner)(uint64_t[], size_t, void *aux), size_t skip, void *aux)
{
  /* RLE fast path */
  if (__builtin_expect(__sm_chunk_is_rle(chunk), 0)) {
    const size_t length = __sm_chunk_rle_get_length(chunk);

    if (skip >= length) {
      return length;
    }

    const size_t scan_start = skip;
    uint64_t buffer[SM_BITS_PER_VECTOR];

    for (size_t i = scan_start; i < length; ) {
      size_t batch_size = SM_BITS_PER_VECTOR;
      if (i + batch_size > length) {
        batch_size = length - i;
      }

      for (size_t j = 0; j < batch_size; j++) {
        buffer[j] = start + i + j;
      }

      scanner(&buffer[0], batch_size, aux);
      i += batch_size;
    }

    return skip;
  }

  size_t pos = 0;
  size_t skipped = 0;
  register uint8_t *p = (uint8_t *)chunk->m_data;
  uint64_t buffer[SM_BITS_PER_VECTOR];
  for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
    if (*p == 0) {
      pos += SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR;
      continue;
    }

    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      const size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        /* No capacity in this slot, do not advance position. */
      } else if (flags == SM_PAYLOAD_ZEROS) {
        pos += SM_BITS_PER_VECTOR;
      } else if (flags == SM_PAYLOAD_ONES) {
        if (skip >= SM_BITS_PER_VECTOR) {
          skip -= SM_BITS_PER_VECTOR;
          skipped += SM_BITS_PER_VECTOR;
          pos += SM_BITS_PER_VECTOR;
        } else if (skip > 0) {
          size_t n = 0;
          for (size_t b = skip; b < SM_BITS_PER_VECTOR; b++) {
            buffer[n++] = start + pos + b;
          }
          skipped += skip;
          skip = 0;
          scanner(&buffer[0], n, aux);
          pos += SM_BITS_PER_VECTOR;
        } else {
          for (size_t b = 0; b < SM_BITS_PER_VECTOR; b++) {
            buffer[b] = start + pos + b;
          }
          scanner(&buffer[0], SM_BITS_PER_VECTOR, aux);
          pos += SM_BITS_PER_VECTOR;
        }
      } else if (flags == SM_PAYLOAD_MIXED) {
        __sm_bitvec_t remaining = chunk->m_data[1 + __sm_chunk_get_position(chunk, (i * SM_FLAGS_PER_INDEX_BYTE) + j)];
        size_t n = 0;
        while (remaining) {
          int b = __builtin_ctzll(remaining);
          if (skip > 0) {
            skip--;
            skipped++;
          } else {
            buffer[n++] = start + pos + b;
          }
          remaining &= remaining - 1;
        }
        if (n > 0) {
          scanner(&buffer[0], n, aux);
        }
        pos += SM_BITS_PER_VECTOR;
      }
    }
  }
  return skipped;
}

/* ----------------------------------------------------------------
 * Chunk alignment
 * ---------------------------------------------------------------- */

static inline __sm_idx_t
__sm_get_chunk_aligned_offset(const size_t idx)
{
  const size_t capacity = SM_CHUNK_MAX_CAPACITY;
  return idx / capacity * capacity;
}

/* ----------------------------------------------------------------
 * Expand / encode pipeline
 * ---------------------------------------------------------------- */

static inline void
__sm_expand_sparse_chunk(const __sm_chunk_t *chunk, __sm_bitvec_t words[32], int cap_flags[32])
{
  const __sm_bitvec_t desc = chunk->m_data[0];

  int vec_offsets[SM_FLAGS_PER_INDEX];
  int running = 0;
  for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
    vec_offsets[i] = running;
    running += (((desc >> (i * 2)) & SM_FLAG_MASK) == SM_PAYLOAD_MIXED);
  }

  for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
    const unsigned f = (desc >> (i * 2)) & SM_FLAG_MASK;
    cap_flags[i] = (f != SM_PAYLOAD_NONE);
    words[i] = (f == SM_PAYLOAD_MIXED) ? chunk->m_data[1 + vec_offsets[i]]
             : (f == SM_PAYLOAD_ONES)  ? ~(__sm_bitvec_t)0
             :                           0;
  }
}

static inline bool
__sm_encode_sparse_chunk(__sm_bitvec_t words[32], int cap_flags[32],
                         __sm_bitvec_t *out_desc, __sm_bitvec_t out_vecs[32], int *out_nvecs)
{
  /* Slot 31 must never be NONE to avoid RLE flag collision. */
  if (!cap_flags[SM_FLAGS_PER_INDEX - 1]) {
    cap_flags[SM_FLAGS_PER_INDEX - 1] = 1;
    words[SM_FLAGS_PER_INDEX - 1] = 0;
  }

  __sm_bitvec_t desc = 0;
  bool has_bits = false;
  unsigned flags[SM_FLAGS_PER_INDEX];
  for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
    unsigned f;
    if (!cap_flags[i]) {
      f = SM_PAYLOAD_NONE;
    } else if (words[i] == 0) {
      f = SM_PAYLOAD_ZEROS;
    } else if (words[i] == ~(__sm_bitvec_t)0) {
      f = SM_PAYLOAD_ONES;
      has_bits = true;
    } else {
      f = SM_PAYLOAD_MIXED;
      has_bits = true;
    }
    flags[i] = f;
    desc |= (__sm_bitvec_t)f << (i * 2);
  }

  int nvecs = 0;
  for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
    if (flags[i] == SM_PAYLOAD_MIXED) {
      out_vecs[nvecs++] = words[i];
    }
  }

  *out_desc = desc;
  *out_nvecs = nvecs;
  return has_bits;
}

static inline void
__sm_expand_rle_as_words(const __sm_chunk_t *rle_chunk, __sm_idx_t rle_start,
                         __sm_idx_t target_start,
                         __sm_bitvec_t words[32], int cap_flags[32],
                         const int *target_cap_flags)
{
  const size_t rle_len = __sm_chunk_rle_get_length(rle_chunk);
  const size_t rle_set_start = (size_t)rle_start;
  const size_t rle_set_end = rle_set_start + rle_len;

  for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
    const size_t slot_start = (size_t)target_start + (size_t)i * SM_BITS_PER_VECTOR;
    const size_t slot_end = slot_start + SM_BITS_PER_VECTOR;

    if (target_cap_flags) {
      cap_flags[i] = target_cap_flags[i];
    } else {
      cap_flags[i] = 1;
    }

    if (slot_end <= rle_set_start || slot_start >= rle_set_end) {
      words[i] = 0;
    } else if (slot_start >= rle_set_start && slot_end <= rle_set_end) {
      words[i] = ~(__sm_bitvec_t)0;
    } else {
      __sm_bitvec_t mask = 0;
      size_t lo = (rle_set_start > slot_start) ? (rle_set_start - slot_start) : 0;
      size_t hi = (rle_set_end < slot_end) ? (rle_set_end - slot_start) : SM_BITS_PER_VECTOR;
      if (hi == SM_BITS_PER_VECTOR) {
        mask = ~((__sm_bitvec_t)0) << lo;
      } else if (lo == 0) {
        mask = ((__sm_bitvec_t)1 << hi) - 1;
      } else {
        mask = (((__sm_bitvec_t)1 << hi) - 1) & (~((__sm_bitvec_t)0) << lo);
      }
      words[i] = mask;
    }
  }
}

static inline void
__sm_merge_carry(__sm_bitvec_t words[32], int cap_flags[32],
                 __sm_bitvec_t carry[32], int carry_cap[32])
{
  for (int i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
    if (carry_cap[i]) {
      words[i] |= carry[i];
      cap_flags[i] = 1;
    }
  }
}

/* ----------------------------------------------------------------
 * SIMD-accelerated word-level operations
 * ---------------------------------------------------------------- */

#if defined(__AVX2__)
#include <immintrin.h>

static inline void
__sm_words_or(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32], const __sm_bitvec_t b[32])
{
  for (int i = 0; i < 32; i += 4) {
    __m256i va = _mm256_loadu_si256((const __m256i *)&a[i]);
    __m256i vb = _mm256_loadu_si256((const __m256i *)&b[i]);
    _mm256_storeu_si256((__m256i *)&dst[i], _mm256_or_si256(va, vb));
  }
}

static inline void
__sm_words_and(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32], const __sm_bitvec_t b[32])
{
  for (int i = 0; i < 32; i += 4) {
    __m256i va = _mm256_loadu_si256((const __m256i *)&a[i]);
    __m256i vb = _mm256_loadu_si256((const __m256i *)&b[i]);
    _mm256_storeu_si256((__m256i *)&dst[i], _mm256_and_si256(va, vb));
  }
}

static inline void
__sm_words_andnot(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32], const __sm_bitvec_t b[32])
{
  /* dst = a & ~b */
  for (int i = 0; i < 32; i += 4) {
    __m256i va = _mm256_loadu_si256((const __m256i *)&a[i]);
    __m256i vb = _mm256_loadu_si256((const __m256i *)&b[i]);
    _mm256_storeu_si256((__m256i *)&dst[i], _mm256_andnot_si256(vb, va));
  }
}

#elif defined(__SSE2__)
#include <emmintrin.h>

static inline void
__sm_words_or(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32], const __sm_bitvec_t b[32])
{
  for (int i = 0; i < 32; i += 2) {
    __m128i va = _mm_loadu_si128((const __m128i *)&a[i]);
    __m128i vb = _mm_loadu_si128((const __m128i *)&b[i]);
    _mm_storeu_si128((__m128i *)&dst[i], _mm_or_si128(va, vb));
  }
}

static inline void
__sm_words_and(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32], const __sm_bitvec_t b[32])
{
  for (int i = 0; i < 32; i += 2) {
    __m128i va = _mm_loadu_si128((const __m128i *)&a[i]);
    __m128i vb = _mm_loadu_si128((const __m128i *)&b[i]);
    _mm_storeu_si128((__m128i *)&dst[i], _mm_and_si128(va, vb));
  }
}

static inline void
__sm_words_andnot(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32], const __sm_bitvec_t b[32])
{
  /* dst = a & ~b */
  for (int i = 0; i < 32; i += 2) {
    __m128i va = _mm_loadu_si128((const __m128i *)&a[i]);
    __m128i vb = _mm_loadu_si128((const __m128i *)&b[i]);
    _mm_storeu_si128((__m128i *)&dst[i], _mm_andnot_si128(vb, va));
  }
}

#else

/* Scalar fallback */
static inline void
__sm_words_or(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32], const __sm_bitvec_t b[32])
{
  for (int i = 0; i < 32; i++) dst[i] = a[i] | b[i];
}

static inline void
__sm_words_and(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32], const __sm_bitvec_t b[32])
{
  for (int i = 0; i < 32; i++) dst[i] = a[i] & b[i];
}

static inline void
__sm_words_andnot(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32], const __sm_bitvec_t b[32])
{
  for (int i = 0; i < 32; i++) dst[i] = a[i] & ~b[i];
}

#endif

#endif /* CHUNK_CODEC_H */
