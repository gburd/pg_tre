/*
 * src/util/surf.c - Succinct Range Filter (SuRF-Base) over trigram keys.
 *
 * LOUDS-Sparse encoding of a byte-trie built over fixed 8-byte big-endian
 * keys.  See include/pg_tre/surf.h for the contract.
 *
 * Trie shape.  Every key is exactly 8 bytes, so the trie has depth <= 8.
 * We build it level by level from the sorted, distinct key array, then
 * emit the three LOUDS-Sparse arrays in BFS (level, then left-to-right)
 * order:
 *
 *   labels[i]     the byte label of edge i
 *   has_child[i]  1 iff edge i descends to an internal node (else it
 *                 terminates a key at this depth)
 *   louds[i]      1 iff edge i is the first edge of its node
 *
 * A "terminal" edge (has_child=0) means some key ends here.  Because all
 * keys are the same length (8), terminals only occur at depth 8; internal
 * structure occupies depths 0..7.  We still handle the general case so the
 * code is robust if variable-length keys are ever fed in.
 *
 * Queries.
 *   may_contain(key): walk the 8 label bytes from the root node; at each
 *     node scan its label range for the byte; follow has_child to the next
 *     node.  Reaching depth 8 on a terminal edge => present.
 *   range_overlaps(lo,hi): walk the trie collecting whether any stored key
 *     lies in [lo,hi].  We do it by finding the smallest stored key >= lo
 *     (successor) and testing key <= hi.  The successor walk uses the same
 *     node navigation plus a "take the next label >= b, else backtrack"
 *     rule.  No false negatives: the structure stores every key in full.
 *
 * rank/select.  Bitmaps are uint64 words with a per-word popcount prefix
 * table (built at load) for O(1)-amortized rank; select is a short scan
 * from the rank block.  Filters here are small (thousands of trigrams), so
 * this is more than fast enough and keeps the code auditable.
 */

#include "postgres.h"

#include "lib/stringinfo.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "pg_tre/surf.h"

#define SURF_KEYLEN 8               /* bytes per key (uint64 big-endian) */
#define SURF_MAGIC  0x53555246u     /* "SURF" */
#define SURF_VERSION 1

/* forward decls */
static bool surf_min_below(const PgTreSurf *s, uint32 node, uint32 pos,
						   uint8 *acc, int d, uint64 *out);

/* ---- succinct bitvector with cached word-prefix ranks ---- */
typedef struct SurfBitvec
{
	uint64	   *words;       /* ceil(nbits/64) words */
	uint32	   *rank;        /* rank[w] = # set bits before word w (nwords+1) */
	uint32		nbits;
	uint32		nwords;
} SurfBitvec;

struct PgTreSurf
{
	uint32		n_labels;    /* total edges (length of the three arrays) */
	uint32		n_nodes;     /* number of trie nodes */
	uint32		n_keys;      /* distinct keys inserted */

	uint8	   *labels;      /* n_labels bytes */
	SurfBitvec	has_child;   /* n_labels bits */
	SurfBitvec	louds;       /* n_labels bits */

	MemoryContext owns;      /* context the arrays live in (NULL if borrowed) */
};

/* ---------- bitvector helpers ---------- */

static void
bv_init(SurfBitvec *bv, uint32 nbits)
{
	bv->nbits = nbits;
	bv->nwords = (nbits + 63) / 64;
	if (bv->nwords == 0)
		bv->nwords = 1;
	bv->words = (uint64 *) palloc0(sizeof(uint64) * bv->nwords);
	bv->rank = NULL;
}

static inline void
bv_set(SurfBitvec *bv, uint32 i)
{
	bv->words[i >> 6] |= (UINT64CONST(1) << (i & 63));
}

static inline bool
bv_get(const SurfBitvec *bv, uint32 i)
{
	return (bv->words[i >> 6] >> (i & 63)) & 1;
}

/* Build the cached word-prefix rank table. */
static void
bv_build_rank(SurfBitvec *bv)
{
	uint32		w;
	uint32		acc = 0;

	bv->rank = (uint32 *) palloc(sizeof(uint32) * (bv->nwords + 1));
	for (w = 0; w < bv->nwords; w++)
	{
		bv->rank[w] = acc;
		acc += pg_popcount64(bv->words[w]);
	}
	bv->rank[bv->nwords] = acc;
}

/* rank1(i) = number of set bits in [0, i).  0 <= i <= nbits. */
static inline uint32
bv_rank1(const SurfBitvec *bv, uint32 i)
{
	uint32		w = i >> 6;
	uint32		off = i & 63;
	uint64		word;

	if (i == 0)
		return 0;
	word = bv->words[w];
	if (off != 0)
		word &= (UINT64CONST(1) << off) - 1;
	else
		word = 0;
	return bv->rank[w] + pg_popcount64(word);
}

/*
 * select1(k): position (0-based) of the k-th set bit, k >= 1.
 * Returns UINT32_MAX if fewer than k set bits.
 */
static inline uint32
bv_select1(const SurfBitvec *bv, uint32 k)
{
	uint32		lo = 0,
				hi = bv->nwords,
				w;
	uint64		word;
	uint32		seen;

	if (k == 0)
		return UINT32_MAX;
	/* Binary search for the word whose prefix rank first reaches k. */
	while (lo < hi)
	{
		uint32		mid = (lo + hi) / 2;

		if (bv->rank[mid + 1] < k)
			lo = mid + 1;
		else
			hi = mid;
	}
	w = lo;
	if (w >= bv->nwords)
		return UINT32_MAX;
	seen = bv->rank[w];
	word = bv->words[w];
	/* find the (k - seen)-th set bit within this word */
	{
		uint32		need = k - seen;	/* >= 1 */
		uint32		bit = 0;

		while (word)
		{
			uint32		tz = pg_rightmost_one_pos64(word);

			need--;
			if (need == 0)
			{
				bit = tz;
				return w * 64 + bit;
			}
			word &= word - 1;
		}
	}
	return UINT32_MAX;
}

/* ---------- LOUDS-Sparse navigation ---------- */

/*
 * A node is identified by the position of its first label (the label index
 * where louds=1).  node_first_label(node) = select1(louds, node+1), where
 * node is 0-based.  The node's labels run until the next louds=1 (or end).
 */
static inline uint32
surf_node_first(const PgTreSurf *s, uint32 node)
{
	return bv_select1(&s->louds, node + 1);
}

static inline uint32
surf_node_end(const PgTreSurf *s, uint32 node)
{
	uint32		nxt = bv_select1(&s->louds, node + 2);

	return (nxt == UINT32_MAX) ? s->n_labels : nxt;
}

/*
 * Child node index reached by descending edge at label position `pos`
 * (requires has_child[pos]=1).  LOUDS-Sparse:
 *   child_node = rank1(has_child, pos+1)     (0-based child among all
 *                                              has_child edges == node id,
 *                                              because node 0 is the root
 *                                              which is not a child).
 * The root is node 0; the first has_child edge leads to node 1, so
 * child_node = rank1(has_child, pos+1).
 */
static inline uint32
surf_child_node(const PgTreSurf *s, uint32 pos)
{
	return bv_rank1(&s->has_child, pos + 1);
}

bool
pg_tre_surf_may_contain(const PgTreSurf *s, uint64 key)
{
	uint8		kb[SURF_KEYLEN];
	uint32		node = 0;
	int			d;

	if (s->n_labels == 0)
		return false;

	for (d = 0; d < SURF_KEYLEN; d++)
		kb[d] = (uint8) (key >> (8 * (SURF_KEYLEN - 1 - d)));

	for (d = 0; d < SURF_KEYLEN; d++)
	{
		uint32		lo = surf_node_first(s, node);
		uint32		hi = surf_node_end(s, node);
		uint32		p;
		bool		found = false;

		for (p = lo; p < hi; p++)
		{
			if (s->labels[p] == kb[d])
			{
				found = true;
				break;
			}
			if (s->labels[p] > kb[d])
				return false;	/* labels sorted; not present */
		}
		if (!found)
			return false;

		if (d == SURF_KEYLEN - 1)
		{
			/* Last byte: a terminal (has_child=0) edge means present. */
			return !bv_get(&s->has_child, p);
		}
		if (!bv_get(&s->has_child, p))
			return false;		/* key longer than stored path */
		node = surf_child_node(s, p);
	}
	return false;
}

/*
 * Find the smallest stored key >= `lo`.  Returns true and sets *out if one
 * exists, false if all stored keys are < lo.  Depth-first successor walk.
 *
 * We descend following the lo-key bytes where possible; when the exact
 * byte is absent at a node we take the next-larger label (and then the
 * minimum path below it); if no larger label exists we backtrack to the
 * nearest ancestor that has a larger sibling.
 */
static bool
surf_successor(const PgTreSurf *s, uint64 lo, uint64 *out)
{
	uint8		kb[SURF_KEYLEN];
	/* Path stack: node id and the label position chosen at each depth. */
	uint32		st_node[SURF_KEYLEN + 1];
	uint32		st_pos[SURF_KEYLEN + 1];
	uint8		acc[SURF_KEYLEN];
	int			d;

	if (s->n_labels == 0)
		return false;

	for (d = 0; d < SURF_KEYLEN; d++)
		kb[d] = (uint8) (lo >> (8 * (SURF_KEYLEN - 1 - d)));

	/* Phase 1: try to match lo exactly, recording the path. */
	{
		uint32		node = 0;

		d = 0;
		for (;;)
		{
			uint32		nlo = surf_node_first(s, node);
			uint32		nhi = surf_node_end(s, node);
			uint32		p;
			bool		matched = false;

			st_node[d] = node;

			for (p = nlo; p < nhi; p++)
			{
				if (s->labels[p] >= kb[d])
				{
					matched = (s->labels[p] == kb[d]);
					break;
				}
			}

			if (p >= nhi)
			{
				/* No label >= kb[d] at this node: backtrack. */
				st_pos[d] = nhi;	/* sentinel: exhausted */
				goto backtrack;
			}

			st_pos[d] = p;
			acc[d] = s->labels[p];

			if (!matched)
			{
				/*
				 * Took a strictly larger label than lo's byte.  The
				 * minimum key under this edge is the successor.
				 */
				return surf_min_below(s, node, p, acc, d, out);
			}

			/* Exact match on kb[d]. */
			if (d == SURF_KEYLEN - 1)
			{
				if (!bv_get(&s->has_child, p))
				{
					/* Exact key present. */
					*out = lo;
					return true;
				}
				/* Shouldn't happen for fixed-length keys, but descend. */
			}
			if (!bv_get(&s->has_child, p))
			{
				/*
				 * lo's path ends at a terminal shorter than 8 (only with
				 * variable-length keys).  The successor is the next label
				 * at some ancestor; backtrack.
				 */
				goto backtrack;
			}
			node = surf_child_node(s, p);
			d++;
			if (d >= SURF_KEYLEN)
				goto backtrack;
		}
	}

backtrack:
	/* Walk up: at each recorded depth, try the next label after st_pos[d]. */
	for (d = d; d >= 0; d--)
	{
		uint32		node = st_node[d];
		uint32		nhi = surf_node_end(s, node);
		uint32		p = st_pos[d];
		uint32		np;

		/* st_pos[d] may be the sentinel (nhi) meaning exhausted. */
		np = (p == nhi) ? nhi : p + 1;
		if (np < nhi)
		{
			acc[d] = s->labels[np];
			return surf_min_below(s, node, np, acc, d, out);
		}
		/* else continue backtracking */
	}
	return false;
}

/*
 * Minimum key in the subtree reached by taking edge at label position `pos`
 * of `node` at depth `d` (acc[0..d] already holds the chosen labels).
 * Follows the leftmost (smallest-label) path down to a terminal.
 */
static bool
surf_min_below(const PgTreSurf *s, uint32 node, uint32 pos,
			   uint8 *acc, int d, uint64 *out)
{
	(void) node;
	for (;;)
	{
		if (!bv_get(&s->has_child, pos))
		{
			/* Terminal edge: acc[0..d] is a full key path. */
			uint64		key = 0;
			int			i;

			if (d != SURF_KEYLEN - 1)
			{
				/*
				 * Terminal shorter than the key length: only possible with
				 * variable-length keys.  Left-pad with zeros to form the
				 * uint64; for pg_tre's fixed 8-byte keys this branch is
				 * never taken.
				 */
			}
			for (i = 0; i <= d; i++)
				key |= ((uint64) acc[i]) << (8 * (SURF_KEYLEN - 1 - i));
			*out = key;
			return true;
		}
		else
		{
			uint32		child = surf_child_node(s, pos);
			uint32		clo = surf_node_first(s, child);

			d++;
			if (d >= SURF_KEYLEN)
				return false;	/* malformed; guard */
			acc[d] = s->labels[clo];
			pos = clo;
			node = child;
		}
	}
}

bool
pg_tre_surf_range_overlaps(const PgTreSurf *s, uint64 lo, uint64 hi)
{
	uint64		succ;

	if (s->n_labels == 0)
		return false;
	if (lo > hi)
		return false;
	if (!surf_successor(s, lo, &succ))
		return false;
	return succ <= hi;
}

/* ---------- build ---------- */

/*
 * Build the trie level by level.  We process the sorted keys and, for each
 * depth, emit one label per distinct (node-prefix, byte) edge in BFS order.
 *
 * Implementation: we maintain, for the current depth, the list of node
 * boundaries as [start,end) ranges over the key array (each range is a set
 * of keys sharing the same d-byte prefix).  For each range we scan the byte
 * at depth d, emitting a label whenever it changes; has_child=1 unless this
 * is the last byte (depth 7) or the group is a single terminating key.
 */
typedef struct
{
	uint32		start;
	uint32		end;
} KeyRange;

PgTreSurf *
pg_tre_surf_build(const uint64 *keys, uint32 n)
{
	PgTreSurf  *s = (PgTreSurf *) palloc0(sizeof(PgTreSurf));
	StringInfoData labels;
	uint8	   *hc_bits;
	uint8	   *ld_bits;
	uint32		cap_bits;
	uint32		n_labels = 0;
	uint32		n_nodes = 0;
	KeyRange   *cur;
	KeyRange   *next;
	uint32		n_cur;
	int			depth;

	s->n_keys = n;
	s->owns = CurrentMemoryContext;

	if (n == 0)
	{
		s->n_labels = 0;
		s->n_nodes = 0;
		s->labels = (uint8 *) palloc(1);
		bv_init(&s->has_child, 0);
		bv_init(&s->louds, 0);
		bv_build_rank(&s->has_child);
		bv_build_rank(&s->louds);
		return s;
	}

	/*
	 * Upper bound on labels: at most n distinct bytes per level * 8 levels,
	 * but tighter is sum over levels of distinct edges <= 8*n.  Allocate
	 * bitmaps to 8*n bits and grow the label buffer dynamically.
	 */
	cap_bits = (n * SURF_KEYLEN) + 64;
	hc_bits = (uint8 *) palloc0((cap_bits + 7) / 8);
	ld_bits = (uint8 *) palloc0((cap_bits + 7) / 8);
	initStringInfo(&labels);

	/* Depth 0: a single root node spanning all keys. */
	cur = (KeyRange *) palloc(sizeof(KeyRange) * (n + 1));
	next = (KeyRange *) palloc(sizeof(KeyRange) * (n + 1));
	cur[0].start = 0;
	cur[0].end = n;
	n_cur = 1;

	for (depth = 0; depth < SURF_KEYLEN && n_cur > 0; depth++)
	{
		uint32		r;
		uint32		n_next = 0;

		for (r = 0; r < n_cur; r++)
		{
			uint32		i = cur[r].start;
			uint32		end = cur[r].end;
			bool		first_in_node = true;

			while (i < end)
			{
				uint8		b = (uint8) (keys[i] >> (8 * (SURF_KEYLEN - 1 - depth)));
				uint32		j = i;
				bool		has_child;

				/* group all keys in [i,end) sharing byte b at this depth */
				while (j < end &&
					   (uint8) (keys[j] >> (8 * (SURF_KEYLEN - 1 - depth))) == b)
					j++;

				/* emit label */
				appendStringInfoChar(&labels, (char) b);
				if (first_in_node)
					ld_bits[n_labels >> 3] |= (1 << (n_labels & 7));

				/*
				 * has_child = 1 unless this is the last key byte (depth 7),
				 * in which case the edge terminates a key.  (All keys are 8
				 * bytes and distinct, so at depth 7 every group is a single
				 * key.)
				 */
				has_child = (depth < SURF_KEYLEN - 1);
				if (has_child)
				{
					hc_bits[n_labels >> 3] |= (1 << (n_labels & 7));
					/* schedule child node over [i,j) for the next depth */
					next[n_next].start = i;
					next[n_next].end = j;
					n_next++;
				}
				n_labels++;
				first_in_node = false;
				i = j;
			}
			n_nodes++;
		}

		/* swap cur/next */
		{
			KeyRange   *tmp = cur;

			cur = next;
			next = tmp;
			n_cur = n_next;
		}
	}

	/* Materialize into the SuRF, building rank caches. */
	s->n_labels = n_labels;
	s->n_nodes = n_nodes;
	s->labels = (uint8 *) palloc(n_labels ? n_labels : 1);
	memcpy(s->labels, labels.data, n_labels);

	bv_init(&s->has_child, n_labels);
	bv_init(&s->louds, n_labels);
	memcpy(s->has_child.words, hc_bits, (n_labels + 7) / 8);
	memcpy(s->louds.words, ld_bits, (n_labels + 7) / 8);
	bv_build_rank(&s->has_child);
	bv_build_rank(&s->louds);

	pfree(labels.data);
	pfree(hc_bits);
	pfree(ld_bits);
	pfree(cur);
	pfree(next);
	return s;
}

void
pg_tre_surf_free(PgTreSurf *s)
{
	if (s == NULL)
		return;
	if (s->labels)
		pfree(s->labels);
	if (s->has_child.words)
		pfree(s->has_child.words);
	if (s->has_child.rank)
		pfree(s->has_child.rank);
	if (s->louds.words)
		pfree(s->louds.words);
	if (s->louds.rank)
		pfree(s->louds.rank);
	pfree(s);
}

uint32
pg_tre_surf_n_keys(const PgTreSurf *s)
{
	return s->n_keys;
}

uint32
pg_tre_surf_n_nodes(const PgTreSurf *s)
{
	return s->n_nodes;
}

/* ---------- serialization ----------
 *
 * Layout (all little-endian, native -- same-endian only, matching the rest
 * of pg_tre's on-disk format):
 *
 *   u32 magic
 *   u32 version
 *   u32 n_labels
 *   u32 n_nodes
 *   u32 n_keys
 *   u8  labels[n_labels]
 *   u64 has_child_words[ceil(n_labels/64)]
 *   u64 louds_words[ceil(n_labels/64)]
 *
 * Rank caches are rebuilt on deserialize (not stored).
 */

Size
pg_tre_surf_serialized_size(const PgTreSurf *s)
{
	uint32		nwords = (s->n_labels + 63) / 64;

	if (nwords == 0)
		nwords = 1;
	return MAXALIGN(5 * sizeof(uint32)) +
		MAXALIGN(s->n_labels) +
		2 * (Size) nwords * sizeof(uint64);
}

void
pg_tre_surf_serialize(const PgTreSurf *s, uint8 *dst)
{
	uint32	   *hdr = (uint32 *) dst;
	uint8	   *p;
	uint32		nwords = (s->n_labels + 63) / 64;

	if (nwords == 0)
		nwords = 1;

	hdr[0] = SURF_MAGIC;
	hdr[1] = SURF_VERSION;
	hdr[2] = s->n_labels;
	hdr[3] = s->n_nodes;
	hdr[4] = s->n_keys;

	p = dst + MAXALIGN(5 * sizeof(uint32));
	memcpy(p, s->labels, s->n_labels);
	p += MAXALIGN(s->n_labels);
	memcpy(p, s->has_child.words, (Size) nwords * sizeof(uint64));
	p += (Size) nwords * sizeof(uint64);
	memcpy(p, s->louds.words, (Size) nwords * sizeof(uint64));
}

PgTreSurf *
pg_tre_surf_deserialize(const uint8 *src, Size len)
{
	const uint32 *hdr = (const uint32 *) src;
	PgTreSurf  *s;
	uint32		n_labels;
	uint32		nwords;
	const uint8 *p;
	Size		need;

	if (len < 5 * sizeof(uint32))
		elog(ERROR, "pg_tre: SuRF image too small (%zu bytes)", len);
	if (hdr[0] != SURF_MAGIC)
		elog(ERROR, "pg_tre: bad SuRF magic 0x%08x", hdr[0]);
	if (hdr[1] != SURF_VERSION)
		elog(ERROR, "pg_tre: unsupported SuRF version %u", hdr[1]);

	n_labels = hdr[2];
	nwords = (n_labels + 63) / 64;
	if (nwords == 0)
		nwords = 1;

	need = MAXALIGN(5 * sizeof(uint32)) + MAXALIGN(n_labels) +
		2 * (Size) nwords * sizeof(uint64);
	if (len < need)
		elog(ERROR, "pg_tre: truncated SuRF image (%zu < %zu)", len, need);

	s = (PgTreSurf *) palloc0(sizeof(PgTreSurf));
	s->owns = CurrentMemoryContext;
	s->n_labels = n_labels;
	s->n_nodes = hdr[3];
	s->n_keys = hdr[4];

	s->labels = (uint8 *) palloc(n_labels ? n_labels : 1);
	p = src + MAXALIGN(5 * sizeof(uint32));
	memcpy(s->labels, p, n_labels);
	p += MAXALIGN(n_labels);

	bv_init(&s->has_child, n_labels);
	bv_init(&s->louds, n_labels);
	memcpy(s->has_child.words, p, (Size) nwords * sizeof(uint64));
	p += (Size) nwords * sizeof(uint64);
	memcpy(s->louds.words, p, (Size) nwords * sizeof(uint64));

	bv_build_rank(&s->has_child);
	bv_build_rank(&s->louds);
	return s;
}
