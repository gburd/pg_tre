/*
 * fuzz/surf_test.c - standalone correctness test for the SuRF filter.
 *
 * Builds a SuRF over random distinct uint64 keys and cross-checks every
 * query against a brute-force sorted array.  Asserts the SuRF contract:
 *   - may_contain(k) is EXACT for SuRF-Base (present iff inserted);
 *   - range_overlaps(lo,hi) has NO false negatives (and, for SuRF-Base
 *     over full keys, is exact).
 *
 * Link: cc surf_test.c ../src/util/surf.c pg_backend_stub.c \
 *          -I../include -I<pg server inc> -lpgcommon -lpgport
 * or just run via the Makefile target `surf-test`.
 */

#include "postgres.h"
#include "pg_tre/surf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* provided by pg_backend_stub.c */
extern jmp_buf *pg_fuzz_error_jmp;

static int
cmp_u64(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* Brute-force: does any key in sorted[] fall in [lo,hi]? */
static bool
bf_overlap(const uint64 *sorted, uint32 n, uint64 lo, uint64 hi)
{
	uint32		i;

	for (i = 0; i < n; i++)
		if (sorted[i] >= lo && sorted[i] <= hi)
			return true;
	return false;
}

static bool
bf_contains(const uint64 *sorted, uint32 n, uint64 k)
{
	uint32		i;

	for (i = 0; i < n; i++)
		if (sorted[i] == k)
			return true;
	return false;
}

/* Restrict keys to the 63-bit trigram-key domain (3 x 21-bit codepoints). */
static uint64
rand_key(void)
{
	uint64		cp0 = (uint64) (random() % 0x110000);
	uint64		cp1 = (uint64) (random() % 0x110000);
	uint64		cp2 = (uint64) (random() % 0x110000);

	return (cp0 << 42) | (cp1 << 21) | cp2;
}

int
main(int argc, char **argv)
{
	int			trial;
	int			ntrials = (argc > 1) ? atoi(argv[1]) : 300;
	unsigned	seed = (argc > 2) ? (unsigned) atoi(argv[2]) : 42;
	long		total_checks = 0;
	jmp_buf		jb;

	pg_fuzz_error_jmp = &jb;
	if (setjmp(jb) != 0)
	{
		fprintf(stderr, "FAIL: backend error (ereport) during test\n");
		return 2;
	}

	srandom(seed);

	for (trial = 0; trial < ntrials; trial++)
	{
		uint32		n = (uint32) (random() % 2000);
		uint64	   *keys = malloc(sizeof(uint64) * (n ? n : 1));
		uint32		i;
		uint32		m;
		PgTreSurf  *s;

		for (i = 0; i < n; i++)
			keys[i] = rand_key();
		qsort(keys, n, sizeof(uint64), cmp_u64);
		/* de-dup */
		m = 0;
		for (i = 0; i < n; i++)
			if (m == 0 || keys[i] != keys[m - 1])
				keys[m++] = keys[i];

		s = pg_tre_surf_build(keys, m);

		/* Round-trip through serialization on half the trials. */
		if (trial & 1)
		{
			Size		len = pg_tre_surf_serialized_size(s);
			uint8	   *buf = malloc(len);

			pg_tre_surf_serialize(s, buf);
			pg_tre_surf_free(s);
			s = pg_tre_surf_deserialize(buf, len);
			free(buf);
		}

		/* Point queries: exact membership on inserted + random keys. */
		for (i = 0; i < m; i++)
		{
			if (!pg_tre_surf_may_contain(s, keys[i]))
			{
				fprintf(stderr, "FAIL trial %d: inserted key %llu missing\n",
						trial, (unsigned long long) keys[i]);
				return 1;
			}
			total_checks++;
		}
		for (i = 0; i < 200; i++)
		{
			uint64		k = rand_key();
			bool		got = pg_tre_surf_may_contain(s, k);
			bool		want = bf_contains(keys, m, k);

			if (got != want)
			{
				fprintf(stderr,
						"FAIL trial %d: contains(%llu) surf=%d bf=%d\n",
						trial, (unsigned long long) k, got, want);
				return 1;
			}
			total_checks++;
		}

		/* Range queries: exact overlap on random intervals. */
		for (i = 0; i < 400; i++)
		{
			uint64		a = rand_key();
			uint64		b = rand_key();
			uint64		lo = (a < b) ? a : b;
			uint64		hi = (a < b) ? b : a;
			bool		got = pg_tre_surf_range_overlaps(s, lo, hi);
			bool		want = bf_overlap(keys, m, lo, hi);

			/* Contract: no false negatives.  SuRF-Base is also exact. */
			if (want && !got)
			{
				fprintf(stderr,
						"FAIL trial %d: FALSE NEGATIVE range [%llu,%llu]\n",
						trial, (unsigned long long) lo,
						(unsigned long long) hi);
				return 1;
			}
			if (got != want)
			{
				fprintf(stderr,
						"FAIL trial %d: overlap [%llu,%llu] surf=%d bf=%d\n",
						trial, (unsigned long long) lo,
						(unsigned long long) hi, got, want);
				return 1;
			}
			total_checks++;
		}

		pg_tre_surf_free(s);
		free(keys);
	}

	printf("OK: %d trials, %ld checks passed\n", ntrials, total_checks);
	return 0;
}
