/*
 * tre_cache.h - Per-session LRU cache of compiled TRE regex patterns.
 *
 * Must be included from a translation unit that includes postgres.h.
 */

#ifndef TRE_CACHE_H
#define TRE_CACHE_H

/*
 * Initialize the cache (idempotent, called from _PG_init or on first use).
 */
void tre_cache_init(void);

/*
 * Look up a compiled regex for the given pattern. Returns the cached
 * compiled handle if found, otherwise compiles and caches it.
 * Raises ereport(ERROR) on compile failure.
 */
void *tre_cache_lookup(const char *pattern, int pattern_len);

#endif /* TRE_CACHE_H */
