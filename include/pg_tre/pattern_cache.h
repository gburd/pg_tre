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
 * Look up a compiled regex for the given pattern under the given
 * collation (which sets what character classes and (?i) mean; see
 * pg_tre_set_collation).  Returns the cached compiled handle if found,
 * otherwise compiles and caches it.  Raises ereport(ERROR) on compile
 * failure and for an invalid or nondeterministic collation.  The caller
 * must call pg_tre_set_collation(collation) again before matching with
 * the handle if anything else may have compiled or matched in between.
 */
void *tre_cache_lookup(const char *pattern, int pattern_len, Oid collation);

/*
 * Like tre_cache_lookup(), but pins the returned compiled handle so the
 * LRU cannot evict/free it until tre_cache_release() is called with the
 * same handle.
 */
void *tre_cache_lookup_pinned(const char *pattern, int pattern_len,
                              Oid collation);

/*
 * Release a pin taken by tre_cache_lookup_pinned(). Matches by pointer
 * identity; a NULL handle is a no-op.
 */
void tre_cache_release(void *compiled);

#endif /* TRE_CACHE_H */
