/* SPDX-License-Identifier: MIT
 *
 * pg_tre/sm_portability.h — internal-only portability shims for the
 * vendored sparsemap implementation.
 *
 * Vendored from sparsemap src/sm_portability.h (upstream commit
 * b409ccc, "build: SM_PREFETCH / SM_POPCOUNT64 / SM_CTZ64 / SM_CLZ64
 * portability shims").  Kept under include/pg_tre/ so src/util/
 * sparsemap.c can reach it via the project's -Iinclude path; the only
 * pg_tre-local change versus upstream is this header comment and the
 * include guard prefix.
 *
 * Wraps four compiler builtins that sparsemap uses in hot paths.  On
 * GCC and Clang they expand to the corresponding `__builtin_*`; on
 * MSVC they expand to the equivalent intrinsic; on unknown compilers
 * they fall back to either a no-op (prefetch) or a portable scalar
 * implementation (popcount / ctz / clz).
 *
 * Macros provided:
 *
 *   SM_PREFETCH(addr)   Hot-loop prefetch hint.  Non-binding; safe
 *                       to drop on toolchains without the intrinsic.
 *   SM_POPCOUNT64(x)    Population count of a 64-bit value.
 *   SM_CTZ64(x)         Count trailing zeros (UB on x == 0; caller
 *                       must guard).
 *   SM_CLZ64(x)         Count leading zeros  (UB on x == 0; caller
 *                       must guard).
 */
#ifndef PG_TRE_SM_PORTABILITY_H
#define PG_TRE_SM_PORTABILITY_H

#include <stdint.h>

/* ---------- SM_PREFETCH ---------- */

#if defined(__GNUC__) || defined(__clang__)
#  define SM_PREFETCH(addr) __builtin_prefetch((addr), 0, 1)
#elif defined(_MSC_VER)
#  include <intrin.h>
#  if defined(_M_ARM64) || defined(_M_ARM)
#    define SM_PREFETCH(addr) __prefetch((const void *)(addr))
#  elif defined(_M_X64) || defined(_M_IX86)
#    define SM_PREFETCH(addr) _mm_prefetch((const char *)(addr), _MM_HINT_T0)
#  else
#    define SM_PREFETCH(addr) ((void)0)
#  endif
#else
#  define SM_PREFETCH(addr) ((void)0)
#endif

/* ---------- SM_POPCOUNT64 ---------- */

#if defined(__GNUC__) || defined(__clang__)
#  define SM_POPCOUNT64(x) ((int)__builtin_popcountll((unsigned long long)(x)))
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#  include <intrin.h>
#  define SM_POPCOUNT64(x) ((int)__popcnt64((unsigned __int64)(x)))
#else
/* SWAR fallback (Sebastiano Vigna, broadword popcount).
 * 12 ops, no table, ~3-5x slower than a hardware popcnt. */
static inline int
sm_swar_popcount64(uint64_t x)
{
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}
#  define SM_POPCOUNT64(x) sm_swar_popcount64((uint64_t)(x))
#endif

/* ---------- SM_CTZ64 ---------- */

#if defined(__GNUC__) || defined(__clang__)
#  define SM_CTZ64(x) __builtin_ctzll((unsigned long long)(x))
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#  include <intrin.h>
static inline int
sm_msvc_ctz64(uint64_t x)
{
    unsigned long idx;
    _BitScanForward64(&idx, (unsigned __int64)x);
    return (int)idx;
}
#  define SM_CTZ64(x) sm_msvc_ctz64((uint64_t)(x))
#else
/* Portable bit-binary-search fallback.  6 branches, no intrinsics. */
static inline int
sm_swar_ctz64(uint64_t x)
{
    int n = 0;
    if (!(x & 0xFFFFFFFFULL)) { n += 32; x >>= 32; }
    if (!(x & 0xFFFFULL))     { n += 16; x >>= 16; }
    if (!(x & 0xFFULL))       { n +=  8; x >>=  8; }
    if (!(x & 0xFULL))        { n +=  4; x >>=  4; }
    if (!(x & 0x3ULL))        { n +=  2; x >>=  2; }
    if (!(x & 0x1ULL))        { n +=  1; }
    return n;
}
#  define SM_CTZ64(x) sm_swar_ctz64((uint64_t)(x))
#endif

/* ---------- SM_CLZ64 ---------- */

#if defined(__GNUC__) || defined(__clang__)
#  define SM_CLZ64(x) __builtin_clzll((unsigned long long)(x))
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#  include <intrin.h>
static inline int
sm_msvc_clz64(uint64_t x)
{
    unsigned long idx;
    _BitScanReverse64(&idx, (unsigned __int64)x);
    return 63 - (int)idx;
}
#  define SM_CLZ64(x) sm_msvc_clz64((uint64_t)(x))
#else
static inline int
sm_swar_clz64(uint64_t x)
{
    int n = 0;
    if (!(x & 0xFFFFFFFF00000000ULL)) { n += 32; x <<= 32; }
    if (!(x & 0xFFFF000000000000ULL)) { n += 16; x <<= 16; }
    if (!(x & 0xFF00000000000000ULL)) { n +=  8; x <<=  8; }
    if (!(x & 0xF000000000000000ULL)) { n +=  4; x <<=  4; }
    if (!(x & 0xC000000000000000ULL)) { n +=  2; x <<=  2; }
    if (!(x & 0x8000000000000000ULL)) { n +=  1; }
    return n;
}
#  define SM_CLZ64(x) sm_swar_clz64((uint64_t)(x))
#endif

#endif /* PG_TRE_SM_PORTABILITY_H */
