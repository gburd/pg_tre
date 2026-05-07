/*
 * Copyright (c) 2024 Gregory Burd <greg@burd.me>.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file sparsemap.h
 * @brief A sparse, compressed bitmap with run-length encoding (RLE).
 *
 * Sparsemap is a mutable, resizable, compressed bitmap optimized for workloads
 * that contain long runs of consecutive set or unset bits.
 *
 * ## Architecture
 *
 * The implementation uses a 3-tier hierarchy:
 *
 * **Tier 0 (bit vectors):** Individual bits are stored in 64-bit words
 * (`uint64_t`).
 *
 * **Tier 1 (chunks):** Groups of bit vectors are managed by chunk maps.
 * Chunks use one of two internal encodings:
 *
 *   - **Sparse encoding:** A descriptor word holds 2-bit flags for up to 32
 *     bit vectors (2048 bits total).  Only vectors with a mix of set and unset
 *     bits are stored; uniform vectors (all-zero or all-one) are represented
 *     by their flag alone:
 *
 *         00  all zeros  -- vector not stored
 *         11  all ones   -- vector not stored
 *         10  mixed      -- vector stored after the descriptor
 *         01  unused     -- reduces chunk capacity
 *
 *   - **RLE encoding:** A single 64-bit descriptor represents a contiguous
 *     run of set bits starting at index 0 within the chunk:
 *
 *         Bits 63:62 = 01  (RLE flag)
 *         Bits 61:31       chunk capacity in bits  (max ~2 billion)
 *         Bits 30:0        run length in bits      (max ~2 billion)
 *
 *     Bits [0, length) are set; bits [length, capacity) are unset.
 *
 * **Tier 2 (map):** The top-level sparsemap manages an ordered sequence of
 * chunks, each tagged with a 4-byte starting offset.  The map grows and
 * shrinks the underlying byte buffer as chunks are added or removed.
 *
 * ## Encoding transitions
 *
 * - A sparse chunk whose vectors are all ones (2048 consecutive set bits)
 *   transitions to RLE when the next adjacent bit is set, extending the run
 *   beyond 2048.
 * - Modifying bits inside an RLE run (clearing a bit in the middle, for
 *   example) causes the RLE chunk to be separated back into one or more
 *   sparse chunks plus (optionally) smaller RLE chunks for the remaining
 *   contiguous runs.
 * - Adjacent chunks (sparse or RLE) that form a contiguous run of set bits
 *   are coalesced into a single RLE chunk automatically.
 *
 * ## Thread safety
 *
 * Sparsemap is **not** thread-safe.  Concurrent reads are safe only when no
 * writer is active.  All mutating operations must be externally synchronized.
 *
 * ## Error handling
 *
 * Functions that mutate the map return `SPARSEMAP_IDX_MAX` and set `errno` to
 * `ENOSPC` when the backing buffer is full.  The caller can grow the buffer
 * with sparsemap_set_data_size() and retry.
 *
 * Allocation functions (sparsemap(), sparsemap_copy(), sparsemap_wrap())
 * return `NULL` on allocation failure.
 */
#ifndef SPARSEMAP_H
#define SPARSEMAP_H

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#if defined(__cplusplus)
extern "C" {
#endif

/** Opaque handle to a sparsemap instance. */
typedef struct sparsemap sparsemap_t;

/** Sentinel value returned when a lookup finds no matching bit. */
#define SPARSEMAP_IDX_MAX UINT64_MAX

/** Evaluates to true when \a x represents a valid (found) index. */
#define SPARSEMAP_FOUND(x) ((x) != SPARSEMAP_IDX_MAX)

/** Evaluates to true when \a x represents the not-found sentinel. */
#define SPARSEMAP_NOT_FOUND(x) ((x) == SPARSEMAP_IDX_MAX)

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/** @brief Allocate a heap-managed sparsemap with an internal buffer.
 *
 * Both the sparsemap_t struct and its data buffer are allocated in a single
 * heap block; a single call to free() releases everything.  The buffer can
 * later be grown via sparsemap_set_data_size(NULL, new_size).
 *
 * @param[in] size  Buffer size in bytes (0 selects a 1024-byte default).
 * @returns A new sparsemap, or NULL on allocation failure.
 *
 * Example:
 * @code
 *   sparsemap_t *map = sparsemap(4096);
 *   sparsemap_add(map, 42);
 *   assert(sparsemap_contains(map, 42));
 *   free(map);
 * @endcode
 */
sparsemap_t *sparsemap(size_t size);

/** @brief Create a deep copy of \a other.
 *
 * @param[in] other  The sparsemap to copy.  Must not be NULL.
 * @returns A new sparsemap with the same contents, or NULL on failure.
 */
sparsemap_t *sparsemap_copy(const sparsemap_t *other);

/** @brief Allocate a sparsemap_t that wraps a caller-provided buffer.
 *
 * The sparsemap_t struct is heap-allocated, but the data buffer is owned by
 * the caller.  The caller must free both the returned handle (free()) and the
 * buffer independently.  Resizing via sparsemap_set_data_size() is only
 * supported when the buffer was allocated together with the struct (see
 * sparsemap()).
 *
 * @param[in] data  Buffer for bitmap storage (stack or heap).
 * @param[in] size  Size of \a data in bytes.
 * @returns A new sparsemap, or NULL on allocation failure.
 */
sparsemap_t *sparsemap_wrap(uint8_t *data, size_t size);

/** @brief Initialize a caller-allocated sparsemap_t with a buffer.
 *
 * Use this when both the sparsemap_t and its buffer are allocated by the
 * caller (e.g. on the stack).  The map is cleared to an empty state.
 *
 * @param[in,out] map   Pointer to an uninitialized sparsemap_t.
 * @param[in]     data  Buffer for bitmap storage.
 * @param[in]     size  Size of \a data in bytes.
 *
 * Example:
 * @code
 *   sparsemap_t map;
 *   uint8_t buf[1024];
 *   sparsemap_init(&map, buf, sizeof(buf));
 *   sparsemap_add(&map, 0);
 * @endcode
 */
void sparsemap_init(sparsemap_t *map, uint8_t *data, size_t size);

/** @brief Attach to an existing (serialized) sparsemap buffer.
 *
 * Unlike sparsemap_init(), this does not clear the buffer.  It calculates the
 * used size from the buffer contents, making it suitable for deserializing a
 * previously-populated bitmap.
 *
 * @param[in,out] map   Pointer to an uninitialized sparsemap_t.
 * @param[in]     data  Buffer containing serialized bitmap data.
 * @param[in]     size  Total capacity of \a data in bytes.
 */
void sparsemap_open(sparsemap_t *map, uint8_t *data, size_t size);

/** @brief Reset the map to empty without freeing memory.
 *
 * After this call the map contains zero set bits but retains its buffer.
 *
 * @param[in,out] map  The sparsemap to clear.
 */
void sparsemap_clear(sparsemap_t *map);

/** @brief Resize the data buffer.
 *
 * When \a data is NULL and the map was created with sparsemap(), the internal
 * buffer is reallocated.  The returned pointer may differ from \a map; the
 * caller must update all references.
 *
 * When \a data is non-NULL the map is re-pointed to the new buffer and
 * \a size becomes the new capacity.  The caller is responsible for copying
 * data if the buffer address changed.
 *
 * @param[in,out] map   The sparsemap to resize.
 * @param[in]     data  New buffer, or NULL to reallocate internally.
 * @param[in]     size  New buffer size in bytes.
 * @returns The (possibly relocated) sparsemap pointer, or NULL on failure.
 */
sparsemap_t *sparsemap_set_data_size(sparsemap_t *map, uint8_t *data, size_t size);

/* -------------------------------------------------------------------
 * Capacity and size
 * ------------------------------------------------------------------- */

/** @brief Estimate remaining buffer capacity as a percentage.
 *
 * Returns a value in [0.0, 100.0].  Because compression ratios change as
 * bits are added or removed, this estimate is non-monotonic -- it may
 * increase after a set() or decrease after an unset().
 *
 * @param[in] map  The sparsemap to query.
 * @returns Estimated percentage of unused buffer space.
 */
double sparsemap_capacity_remaining(const sparsemap_t *map);

/** @brief Return the total buffer capacity in bytes.
 *
 * This is the \a size value provided at construction, not the number of
 * indexable bits.
 *
 * @param[in] map  The sparsemap to query.
 * @returns Buffer capacity in bytes.
 */
size_t sparsemap_get_capacity(const sparsemap_t *map);

/** @brief Return the number of buffer bytes currently in use.
 *
 * Useful for serialization: only the first sparsemap_get_size() bytes of the
 * buffer returned by sparsemap_get_data() need to be persisted.
 *
 * @param[in] map  The sparsemap to query.
 * @returns Used byte count.
 */
size_t sparsemap_get_size(sparsemap_t *map);

/** @brief Return a pointer to the raw data buffer.
 *
 * The first sparsemap_get_size() bytes contain the serialized bitmap; the
 * remainder (up to sparsemap_get_capacity()) is unused.
 *
 * @param[in] map  The sparsemap to query.
 * @returns Pointer to the data buffer.
 */
void *sparsemap_get_data(const sparsemap_t *map);

/* -------------------------------------------------------------------
 * Single-bit operations
 * ------------------------------------------------------------------- */

/** @brief Test whether the bit at \a idx is set.
 *
 * @param[in] map  The sparsemap to query.
 * @param[in] idx  0-based bit position.
 * @returns true if bit \a idx is 1, false if 0 or out of range.
 */
bool sparsemap_contains(sparsemap_t *map, uint64_t idx);

/** @brief Set or clear the bit at \a idx.
 *
 * Equivalent to `value ? sparsemap_add(map, idx) : sparsemap_remove(map, idx)`.
 *
 * @param[in,out] map    The sparsemap to modify.
 * @param[in]     idx    0-based bit position.
 * @param[in]     value  true to set, false to clear.
 * @returns \a idx on success, or SPARSEMAP_IDX_MAX with errno=ENOSPC.
 *
 * Example:
 * @code
 *   sparsemap_assign(map, 100, true);   // set bit 100
 *   sparsemap_assign(map, 100, false);  // clear bit 100
 * @endcode
 */
uint64_t sparsemap_assign(sparsemap_t *map, uint64_t idx, bool value);

/** @brief Set the bit at \a idx to 1.
 *
 * If the buffer is full, returns SPARSEMAP_IDX_MAX and sets errno to ENOSPC.
 * Grow the buffer with sparsemap_set_data_size() and retry.
 *
 * Setting a bit may trigger chunk coalescing: if the new bit extends a
 * contiguous run of set bits across chunk boundaries, adjacent chunks may be
 * merged into a single RLE chunk.
 *
 * @param[in,out] map  The sparsemap to modify.
 * @param[in]     idx  0-based bit position to set.
 * @returns \a idx on success, or SPARSEMAP_IDX_MAX with errno=ENOSPC.
 *
 * Example:
 * @code
 *   uint64_t r = sparsemap_add(map, 42);
 *   if (SPARSEMAP_NOT_FOUND(r)) {
 *       map = sparsemap_set_data_size(map, NULL, new_size);
 *       sparsemap_add(map, 42);
 *   }
 * @endcode
 */
uint64_t sparsemap_add(sparsemap_t *map, uint64_t idx);

/** @brief Clear the bit at \a idx (set to 0).
 *
 * Clearing a bit inside an RLE run causes the RLE chunk to be separated
 * into sparse and/or smaller RLE chunks.  This may temporarily increase
 * buffer usage even though a bit was removed.
 *
 * If the buffer is full (insufficient space for the new chunk layout),
 * returns SPARSEMAP_IDX_MAX and sets errno to ENOSPC.
 *
 * @param[in,out] map  The sparsemap to modify.
 * @param[in]     idx  0-based bit position to clear.
 * @returns \a idx on success, or SPARSEMAP_IDX_MAX with errno=ENOSPC.
 */
uint64_t sparsemap_remove(sparsemap_t *map, uint64_t idx);

/* -------------------------------------------------------------------
 * Aggregate queries
 * ------------------------------------------------------------------- */

/** @brief Count the total number of set bits (cardinality).
 *
 * Equivalent to `sparsemap_rank(map, 0, SPARSEMAP_IDX_MAX, true)`.
 *
 * @param[in] map  The sparsemap to query.
 * @returns Number of bits that are set to 1.
 */
size_t sparsemap_cardinality(sparsemap_t *map);

/** @brief Return the position of the first set bit (minimum).
 *
 * @param[in] map  The sparsemap to query.
 * @returns 0-based index of the lowest set bit, or 0 if the map is empty.
 */
uint64_t sparsemap_minimum(const sparsemap_t *map);

/** @brief Return the position of the last set bit (maximum).
 *
 * @param[in] map  The sparsemap to query.
 * @returns 0-based index of the highest set bit, or 0 if the map is empty.
 */
uint64_t sparsemap_maximum(const sparsemap_t *map);

/** @brief Return the fraction of bits that are set.
 *
 * Computed as cardinality / (maximum - minimum + 1).
 *
 * @param[in] map  The sparsemap to query.
 * @returns Fill factor in the range [0.0, 1.0].
 */
double sparsemap_fill_factor(sparsemap_t *map);

/* -------------------------------------------------------------------
 * Rank, select, and span
 * ------------------------------------------------------------------- */

/** @brief Count matching bits in the inclusive range [\a x, \a y].
 *
 * @param[in] map    The sparsemap to query.
 * @param[in] x      0-based start of range (inclusive).
 * @param[in] y      0-based end of range (inclusive).
 * @param[in] value  true to count set bits, false to count unset bits.
 * @returns Number of bits matching \a value in the range.
 *
 * Example:
 * @code
 *   // Count set bits in positions [100, 199]
 *   size_t n = sparsemap_rank(map, 100, 199, true);
 * @endcode
 */
size_t sparsemap_rank(sparsemap_t *map, uint64_t x, uint64_t y, bool value);

/** @brief Find the position of the \a n'th matching bit (0-based).
 *
 * select(map, 0, true) returns the index of the first set bit.
 * select(map, 2, false) returns the index of the third unset bit.
 *
 * For RLE chunks this is O(1); for sparse chunks it scans bit vectors.
 *
 * @param[in] map    The sparsemap to query.
 * @param[in] n      Number of matching bits to skip (0 = first match).
 * @param[in] value  true to find set bits, false to find unset bits.
 * @returns 0-based index of the matching bit, or SPARSEMAP_IDX_MAX if
 *          fewer than n+1 matching bits exist.
 *
 * Example:
 * @code
 *   uint64_t first_set  = sparsemap_select(map, 0, true);
 *   uint64_t third_zero = sparsemap_select(map, 2, false);
 * @endcode
 */
uint64_t sparsemap_select(sparsemap_t *map, uint64_t n, bool value);

/** @brief Find the first contiguous run of \a len bits matching \a value.
 *
 * Searches forward from \a start for a span of at least \a len consecutive
 * bits that all match \a value.
 *
 * @param[in] map    The sparsemap to search.
 * @param[in] start  0-based position to begin searching.
 * @param[in] len    Required run length.
 * @param[in] value  true to find set bits, false to find unset bits.
 * @returns 0-based index of the first bit in the run, or SPARSEMAP_IDX_MAX
 *          if no such run exists.
 */
uint64_t sparsemap_span(sparsemap_t *map, uint64_t start, size_t len, bool value);

/* -------------------------------------------------------------------
 * Iteration
 * ------------------------------------------------------------------- */

/** @brief Invoke a callback for every set bit in the map.
 *
 * The callback receives batches of up to 64 indices at a time.  Indices are
 * delivered in ascending order.
 *
 * @param[in] map      The sparsemap to scan.
 * @param[in] scanner  Callback invoked with (array_of_indices, count, aux).
 * @param[in] skip     Number of set bits to skip before invoking the callback.
 * @param[in] aux      Opaque pointer forwarded to \a scanner.
 *
 * Example:
 * @code
 *   void print_bits(uint32_t idx[], size_t n, void *aux) {
 *       for (size_t i = 0; i < n; i++)
 *           printf("%u\n", idx[i]);
 *   }
 *   sparsemap_scan(map, print_bits, 0, NULL);
 * @endcode
 */
void sparsemap_scan(const sparsemap_t *map, void (*scanner)(uint64_t vec[], size_t n, void *aux), size_t skip, void *aux);

/* -------------------------------------------------------------------
 * Bulk operations
 * ------------------------------------------------------------------- */

/** @brief Create a new sparsemap containing bits set in either \a a or \a b.
 *
 * The result is a newly allocated sparsemap whose set bits are exactly those
 * that appear in either input map (logical OR).  Neither input is modified.
 *
 * @param[in] a  First input sparsemap.
 * @param[in] b  Second input sparsemap.
 * @returns A newly allocated sparsemap (caller must free()), or NULL on
 *          allocation failure or if both inputs are empty/NULL.
 */
sparsemap_t *sparsemap_union(const sparsemap_t *a, const sparsemap_t *b);

/** @brief Create a new sparsemap containing bits set in both \a a and \a b.
 *
 * The result is a newly allocated sparsemap whose set bits are exactly those
 * that appear in both input maps (logical AND).  Neither input is modified.
 *
 * @param[in] a  First input sparsemap.
 * @param[in] b  Second input sparsemap.
 * @returns A newly allocated sparsemap (caller must free()), or NULL on
 *          allocation failure or if the result would be empty.
 */
sparsemap_t *sparsemap_intersection(const sparsemap_t *a, const sparsemap_t *b);

/** @brief Create a new sparsemap containing bits set in \a a but not in \a b.
 *
 * The result is a newly allocated sparsemap whose set bits are exactly those
 * that appear in \a a but not in \a b (logical AND NOT).  Neither input is
 * modified.
 *
 * @param[in] a  First input sparsemap (minuend).
 * @param[in] b  Second input sparsemap (subtrahend).
 * @returns A newly allocated sparsemap (caller must free()), or NULL on
 *          allocation failure or if the result would be empty.
 */
sparsemap_t *sparsemap_difference(const sparsemap_t *a, const sparsemap_t *b);

/** @brief Split the map at \a idx, moving higher bits to \a other.
 *
 * After the split, \a map contains bits in [start, idx) and \a other
 * contains bits in [idx, end].  The \a other map must be empty on entry.
 *
 * When \a idx is SPARSEMAP_IDX_MAX, the map is split at the median set
 * bit, producing two halves of roughly equal cardinality.
 *
 * If the split crosses an RLE chunk, that chunk is first separated into
 * sparse/RLE pieces, then the split proceeds on the resulting sparse chunk.
 * Adjacent chunks that form contiguous runs are coalesced after the split.
 *
 * @param[in,out] map    Source map (retains [start, idx)).
 * @param[in]     idx    Split point, or SPARSEMAP_IDX_MAX for even split.
 * @param[in,out] other  Destination for [idx, end] (must be empty).
 * @returns The index at which the map was split, or SPARSEMAP_IDX_MAX with
 *          errno=ENOSPC if the buffer is too small.
 *
 * Example:
 * @code
 *   sparsemap_t *left = sparsemap(4096);
 *   sparsemap_t *right = sparsemap(4096);
 *   // populate left ...
 *   sparsemap_split(left, SPARSEMAP_IDX_MAX, right);
 *   // left has the lower half, right has the upper half
 * @endcode
 */
uint64_t sparsemap_split(sparsemap_t *map, uint64_t idx, sparsemap_t *other);

/** @brief Create a new sparsemap with all bits shifted by \a offset.
 *
 * Every set bit at position i in \a map appears at position i + offset in
 * the result.  Bits shifted below 0 are silently dropped (matching
 * CRoaring and PostgreSQL semantics).
 *
 * @param[in] map     The source sparsemap.
 * @param[in] offset  Signed shift amount (positive = right, negative = left).
 * @returns A newly allocated sparsemap (caller must free()), or NULL if all
 *          bits are shifted away or on allocation failure.
 */
sparsemap_t *sparsemap_offset(const sparsemap_t *map, ssize_t offset);

#if defined(__cplusplus)
}
#endif

#endif /* !defined(SPARSEMAP_H) */
