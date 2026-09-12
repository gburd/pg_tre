/*
 * src/pages/surf_page.c - persist/read the SuRF filter across SURF pages.
 *
 * The serialized SuRF image (src/util/surf.c) is written to a chain of
 * PG_TRE_PAGE_SURF pages starting at meta.root_surf; each page holds a
 * PgTreSurfHeader + a chunk of the image.  Read concatenates the chunks and
 * hands the bytes to pg_tre_surf_deserialize.
 *
 * SuRF is rebuilt wholesale on every index build (it is a whole-index
 * structure), so there is no incremental update path -- ambuild writes the
 * chain once and stamps meta.root_surf.
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/generic_xlog.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/pg_am.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lockdefs.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "pg_tre/buffer.h"
#include "pg_tre/meta.h"
#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/surf.h"
#include "pg_tre/surf_page.h"
#include "pg_tre/xlog.h"

/* Usable image bytes per SURF page (content area minus the header). */
static inline Size
surf_page_capacity(void)
{
	Size		content = BLCKSZ - MAXALIGN(SizeOfPageHeaderData)
		- MAXALIGN(sizeof(PageTreOpaqueData));

	return content - MAXALIGN(sizeof(PgTreSurfHeader));
}

/*
 * Write `image` (len bytes) across a fresh chain of SURF pages.  Returns
 * the first page's block number, or InvalidBlockNumber if len == 0.
 */
BlockNumber
pg_tre_surf_write_image(Relation index, const uint8 *image, Size len)
{
	Size		cap = surf_page_capacity();
	Size		off = 0;
	BlockNumber first = InvalidBlockNumber;
	Buffer		prev_buf = InvalidBuffer;
	bool		is_first = true;

	if (len == 0)
		return InvalidBlockNumber;

	while (off < len || is_first)
	{
		Buffer		buf;
		Page		page;
		char	   *content;
		PgTreSurfHeader *hdr;
		Size		chunk;
		BlockNumber this_blk;
		GenericXLogState *state;

		buf = pg_tre_extend(index, PG_TRE_PAGE_SURF);
		this_blk = BufferGetBlockNumber(buf);

		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, buf, 0);
		content = (char *) PageGetContents(page);

		chunk = Min(cap, len - off);

		hdr = (PgTreSurfHeader *) content;
		hdr->next_page = InvalidBlockNumber;	/* patched on the next iter */
		hdr->chunk_bytes = (uint32) chunk;
		hdr->total_bytes = is_first ? (uint32) len : 0;
		hdr->_pad0 = 0;
		memcpy(content + MAXALIGN(sizeof(PgTreSurfHeader)), image + off, chunk);

		((PageHeader) page)->pd_lower =
			(LocationIndex) (content + MAXALIGN(sizeof(PgTreSurfHeader)) + chunk
							 - (char *) page);

		GenericXLogFinish(state);

		/* Link the previous page to this one. */
		if (prev_buf != InvalidBuffer)
		{
			Page		ppage;
			PgTreSurfHeader *phdr;

			state = GenericXLogStart(index);
			ppage = GenericXLogRegisterBuffer(state, prev_buf, 0);
			phdr = (PgTreSurfHeader *) PageGetContents(ppage);

			phdr->next_page = this_blk;
			GenericXLogFinish(state);
			UnlockReleaseBuffer(prev_buf);
		}

		if (is_first)
			first = this_blk;

		prev_buf = buf;
		off += chunk;
		is_first = false;

		CHECK_FOR_INTERRUPTS();
	}

	if (prev_buf != InvalidBuffer)
		UnlockReleaseBuffer(prev_buf);

	return first;
}

/*
 * Read the SuRF image from the chain rooted at `root` and deserialize it.
 * Returns NULL if root is InvalidBlockNumber (no filter).  The returned
 * PgTreSurf is palloc'd in the current memory context.
 */
PgTreSurf *
pg_tre_surf_read(Relation index, BlockNumber root)
{
	Buffer		buf;
	Page		page;
	PgTreSurfHeader *hdr;
	uint8	   *image;
	Size		total;
	Size		off = 0;
	BlockNumber blk = root;
	PgTreSurf  *s;

	if (root == InvalidBlockNumber)
		return NULL;

	/* First page carries total_bytes; allocate the full image buffer. */
	buf = pg_tre_read(index, blk, PG_TRE_PAGE_SURF, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	hdr = (PgTreSurfHeader *) PageGetContents(page);
	total = hdr->total_bytes;
	if (total == 0)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "pg_tre: SuRF first page has zero total_bytes");
	}
	image = (uint8 *) palloc(total);

	for (;;)
	{
		Size		chunk = hdr->chunk_bytes;
		BlockNumber next = hdr->next_page;

		if (off + chunk > total)
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR, "pg_tre: SuRF chain overflows declared length");
		}
		memcpy(image + off,
			   (char *) PageGetContents(page) + MAXALIGN(sizeof(PgTreSurfHeader)),
			   chunk);
		off += chunk;
		UnlockReleaseBuffer(buf);

		if (next == InvalidBlockNumber)
			break;
		blk = next;
		buf = pg_tre_read(index, blk, PG_TRE_PAGE_SURF, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		hdr = (PgTreSurfHeader *) PageGetContents(page);
		CHECK_FOR_INTERRUPTS();
	}

	if (off != total)
		elog(ERROR, "pg_tre: SuRF chain short (%zu of %zu bytes)", off, total);

	s = pg_tre_surf_deserialize(image, total);
	pfree(image);
	return s;
}

/*
 * tre_surf_stats(idx regclass) RETURNS (n_keys int8, n_nodes int8,
 *                                        n_pages int4, image_bytes int8)
 *
 * Introspection for tests / EXPLAIN: reports the SuRF filter's key count,
 * trie node count, on-disk page count, and serialized image size.  All
 * columns are 0 (and n_pages 0) when the index has no SuRF (pre-v10 or
 * empty index).
 */
PG_FUNCTION_INFO_V1(tre_surf_stats);
Datum
tre_surf_stats(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Relation	index;
	PgTreMetaPageData meta;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};
	int64		n_keys = 0;
	int64		n_nodes = 0;
	int32		n_pages = 0;
	int64		image_bytes = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "tre_surf_stats: return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	index = index_open(indexoid, AccessShareLock);
	pg_tre_meta_read(index, &meta);

	if (meta.root_surf != InvalidBlockNumber)
	{
		PgTreSurf  *s = pg_tre_surf_read(index, meta.root_surf);

		if (s != NULL)
		{
			BlockNumber blk = meta.root_surf;

			n_keys = pg_tre_surf_n_keys(s);
			n_nodes = pg_tre_surf_n_nodes(s);
			image_bytes = (int64) pg_tre_surf_serialized_size(s);
			pg_tre_surf_free(s);

			/* Count pages in the chain. */
			while (blk != InvalidBlockNumber)
			{
				Buffer		buf = pg_tre_read(index, blk, PG_TRE_PAGE_SURF,
											  BUFFER_LOCK_SHARE);
				PgTreSurfHeader *hdr =
					(PgTreSurfHeader *) PageGetContents(BufferGetPage(buf));
				BlockNumber next = hdr->next_page;

				n_pages++;
				UnlockReleaseBuffer(buf);
				blk = next;
			}
		}
	}

	index_close(index, AccessShareLock);

	values[0] = Int64GetDatum(n_keys);
	values[1] = Int64GetDatum(n_nodes);
	values[2] = Int32GetDatum(n_pages);
	values[3] = Int64GetDatum(image_bytes);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
