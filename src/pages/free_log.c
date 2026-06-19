/*
 * src/pages/free_log.c - deferred page-free log (Blocker 2).
 *
 * See include/pg_tre/free_log.h for the why.  In short: a VACUUM-time
 * merge that drops a run cannot free the run's pages promptly, because a
 * concurrent scan (VACUUM holds only ShareUpdateExclusiveLock) may still
 * be traversing them via a run root it captured before the catalog
 * rewrite.  So we log (block, deletion-XID) here without touching the
 * page, and a later VACUUM reclaims each block once its XID has aged
 * past the global removable horizon -- exactly nbtree's safexid gate,
 * the same discipline pg_tre_posting_recycle_deleted uses for unlinked
 * posting leaves.
 *
 * WAL: both the append and the per-block reclaim are full-page-image
 * records under XLOG_PTRE_FREE_LOG, replayed by the generic FPI redo.
 * Each reclaim bundles the freed block's blank reinit AND the log-page
 * entry removal in ONE record, so replay and a re-run after a crash are
 * idempotent (a half-done reclaim cannot leave a freed-but-still-logged
 * block, nor a logged-but-reused block).
 */

#include "postgres.h"

#include <string.h>

#include "access/transam.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/lockdefs.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "pg_tre/buffer.h"
#include "pg_tre/coalesced.h"
#include "pg_tre/free_log.h"
#include "pg_tre/meta.h"
#include "pg_tre/page.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/xlog.h"

/*
 * Local mirror of upper.c's file-private internal-page entry layout
 * (must match PgTreUpperInternalEntry exactly), as pending.c and
 * posting.c also mirror it, so we can descend a multi-level upper tree.
 */
typedef struct PgTreFreeLogUpperInternalEntry
{
	uint64		first_key;
	BlockNumber child_blk;
} PgTreFreeLogUpperInternalEntry;

/* Max PgTreFreeLogEntry records per log page after the header. */
#define FREE_LOG_CAP                                  \
	((int)((BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - \
			MAXALIGN(sizeof(PageTreOpaqueData)) -     \
			MAXALIGN(sizeof(PgTreFreeLogHeader))) /   \
		   sizeof(PgTreFreeLogEntry)))

static inline PgTreFreeLogHeader *
free_log_header(Page page)
{
	return (PgTreFreeLogHeader *)PageGetContents(page);
}

static inline PgTreFreeLogEntry *
free_log_entries(Page page)
{
	return (PgTreFreeLogEntry *)((char *)PageGetContents(page) +
								 MAXALIGN(sizeof(PgTreFreeLogHeader)));
}

/* --------------------------------------------------------------------
 * Run-page collection (read-only)
 * -------------------------------------------------------------------- */

typedef struct CollectState
{
	BlockNumber	 *blocks;
	int			  n;
	int			  cap;
	MemoryContext mcxt;
} CollectState;

static void
collect_add(CollectState *cs, BlockNumber blk)
{
	if (!BlockNumberIsValid(blk) || blk == PG_TRE_META_BLKNO)
		return;
	if (cs->n >= cs->cap)
	{
		int			  newcap = (cs->cap == 0) ? 64 : cs->cap * 2;
		MemoryContext old	 = MemoryContextSwitchTo(cs->mcxt);
		if (cs->blocks == NULL)
			cs->blocks = palloc(newcap * sizeof(BlockNumber));
		else
			cs->blocks = repalloc(cs->blocks, newcap * sizeof(BlockNumber));
		MemoryContextSwitchTo(old);
		cs->cap = newcap;
	}
	cs->blocks[cs->n++] = blk;
}

/* Walk a posting-tree right-link chain rooted at `root`, adding each
 * leaf block.  No-op when root is invalid (inline posting). */
static void
collect_posting_chain(Relation index, CollectState *cs, BlockNumber root)
{
	BlockNumber blk	  = root;
	int			guard = 0;

	while (BlockNumberIsValid(blk))
	{
		Buffer		  buf;
		Page		  page;
		PageTreOpaque opq;
		BlockNumber	  next;

		CHECK_FOR_INTERRUPTS();
		if (++guard > (int)RelationGetNumberOfBlocks(index) + 1)
			break; /* defensive: corrupt/cyclic chain */

		buf	 = pg_tre_read(index, blk, PG_TRE_PAGE_INVALID, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		opq	 = PageTreGetOpaque(page);
		if (opq->page_kind != PG_TRE_PAGE_POSTING_L)
		{
			UnlockReleaseBuffer(buf);
			break;
		}
		collect_add(cs, blk);
		next = ((PgTrePostingLeafHeader *)PageGetContents(page))->right_link;
		UnlockReleaseBuffer(buf);
		blk = next;
	}
}

static void
collect_upper_subtree(Relation index, CollectState *cs, BlockNumber blk)
{
	Buffer		  buf;
	Page		  page;
	PageTreOpaque opq;

	CHECK_FOR_INTERRUPTS();

	buf	 = pg_tre_read(index, blk, PG_TRE_PAGE_INVALID, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	opq	 = PageTreGetOpaque(page);

	if (opq->page_kind == PG_TRE_PAGE_UPPER_L)
	{
		PgTreUpperLeafEntry *ents =
				(PgTreUpperLeafEntry *)PageGetContents(page);
		int			 n = opq->flags;
		int			 i;
		BlockNumber *roots;
		int			 n_roots = 0;

		if (n == 0)
			n = (((PageHeader)page)->pd_lower - sizeof(PageHeaderData)) /
				sizeof(PgTreUpperLeafEntry);

		/*
		 * Snapshot the out-of-line posting-tree roots into mcxt storage
		 * BEFORE releasing the leaf lock; `ents` points into the page
		 * buffer, so it must not be dereferenced after unlock.  Coalesced
		 * pages (the shared posting_root block) are added directly here;
		 * out-of-line roots are walked by collect_posting_chain after the
		 * leaf is released (so we never hold two index buffers locked).
		 */
		roots = (n > 0) ? MemoryContextAlloc(
								  cs->mcxt, (Size)n * sizeof(BlockNumber))
						: NULL;
		for (i = 0; i < n; i++)
		{
			BlockNumber root = ents[i].posting_root;
			uint32		ib	 = ents[i].inline_bytes;

			if ((ib & PG_TRE_COALESCED_FLAG) != 0)
				collect_add(cs, root); /* shared coalesced page */
			else if (ib == 0 && BlockNumberIsValid(root))
				roots[n_roots++] = root; /* posting-tree root, walked below */
		}
		/* The leaf page itself is part of the dropped run. */
		collect_add(cs, blk);

		UnlockReleaseBuffer(buf);

		for (i = 0; i < n_roots; i++)
			collect_posting_chain(index, cs, roots[i]);
		if (roots != NULL)
			pfree(roots);
		return;
	}

	if (opq->page_kind != PG_TRE_PAGE_UPPER)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR,
			 "pg_tre: unexpected page kind %u while collecting run pages",
			 opq->page_kind);
	}

	/* Internal page: collect children, release, recurse. */
	collect_add(cs, blk);
	{
		PgTreFreeLogUpperInternalEntry *ents =
				(PgTreFreeLogUpperInternalEntry *)PageGetContents(page);
		int n = (((PageHeader)page)->pd_lower - sizeof(PageHeaderData)) /
				sizeof(PgTreFreeLogUpperInternalEntry);
		BlockNumber *children;
		int			 i;

		if (n <= 0)
		{
			UnlockReleaseBuffer(buf);
			return;
		}
		children = MemoryContextAlloc(cs->mcxt, (Size)n * sizeof(BlockNumber));
		for (i = 0; i < n; i++)
			children[i] = ents[i].child_blk;
		UnlockReleaseBuffer(buf);

		for (i = 0; i < n; i++)
			collect_upper_subtree(index, cs, children[i]);
		pfree(children);
	}
}

static int
blk_cmp(const void *a, const void *b)
{
	BlockNumber x = *(const BlockNumber *)a;
	BlockNumber y = *(const BlockNumber *)b;
	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

BlockNumber *
pg_tre_run_collect_pages(Relation index, BlockNumber root_upper, int *out_n)
{
	CollectState cs;
	int			 w, r;

	memset(&cs, 0, sizeof(cs));
	cs.mcxt = CurrentMemoryContext;

	if (!BlockNumberIsValid(root_upper))
	{
		*out_n = 0;
		return NULL;
	}

	collect_upper_subtree(index, &cs, root_upper);

	if (cs.n == 0)
	{
		*out_n = 0;
		return NULL;
	}

	/* Dedup (coalesced pages are referenced by many upper entries). */
	qsort(cs.blocks, cs.n, sizeof(BlockNumber), blk_cmp);
	for (w = 0, r = 0; r < cs.n; r++)
		if (r == 0 || cs.blocks[r] != cs.blocks[w - 1])
			cs.blocks[w++] = cs.blocks[r];
	cs.n = w;

	*out_n = cs.n;
	return cs.blocks;
}

/* --------------------------------------------------------------------
 * Append
 * -------------------------------------------------------------------- */

/* Append entries[base..base+take) onto one fresh-or-existing head page
 * in a single WAL'd critical section; returns how many were written. */
static int
free_log_append_chunk(
		Relation		   index,
		const BlockNumber *blocks,
		int				   base,
		int				   n,
		uint64			   del_xid_value)
{
	Buffer				metabuf, catbuf;
	Page				metapage, catpage;
	PgTreMetaPage		meta;
	PgTreFreeLogHeader *hdr;
	PgTreFreeLogEntry  *ents;
	int					take, i;
	bool				new_page = false;

	metabuf = pg_tre_read(
			index, PG_TRE_META_BLKNO, PG_TRE_PAGE_META, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	meta	 = PgTreMetaPageGet(metapage);
	if (meta->free_log_head == 0)
		meta->free_log_head = InvalidBlockNumber;

	if (meta->free_log_head == InvalidBlockNumber)
	{
		catbuf			= pg_tre_extend(index, PG_TRE_PAGE_FREE_LOG);
		catpage			= BufferGetPage(catbuf);
		hdr				= free_log_header(catpage);
		hdr->right_link = InvalidBlockNumber;
		hdr->n_entries	= 0;
		hdr->_pad0		= 0;
		((PageHeader)catpage)->pd_lower =
				(char *)free_log_entries(catpage) - (char *)catpage;
		new_page = true;
	}
	else
	{
		catbuf = pg_tre_read(
				index,
				meta->free_log_head,
				PG_TRE_PAGE_FREE_LOG,
				BUFFER_LOCK_EXCLUSIVE);
		catpage = BufferGetPage(catbuf);
		hdr		= free_log_header(catpage);

		if ((int)hdr->n_entries >= FREE_LOG_CAP)
		{
			BlockNumber old_head = meta->free_log_head;
			UnlockReleaseBuffer(catbuf);
			catbuf			= pg_tre_extend(index, PG_TRE_PAGE_FREE_LOG);
			catpage			= BufferGetPage(catbuf);
			hdr				= free_log_header(catpage);
			hdr->right_link = old_head;
			hdr->n_entries	= 0;
			hdr->_pad0		= 0;
			((PageHeader)catpage)->pd_lower =
					(char *)free_log_entries(catpage) - (char *)catpage;
			new_page = true;
		}
	}

	ents = free_log_entries(catpage);
	take = n - base;
	if (take > FREE_LOG_CAP - (int)hdr->n_entries)
		take = FREE_LOG_CAP - (int)hdr->n_entries;

	START_CRIT_SECTION();

	for (i = 0; i < take; i++)
	{
		ents[hdr->n_entries + i].block		   = blocks[base + i];
		ents[hdr->n_entries + i]._pad0		   = 0;
		ents[hdr->n_entries + i].del_xid_value = del_xid_value;
	}
	hdr->n_entries += (uint32)take;
	((PageHeader)catpage)->pd_lower =
			(char *)&ents[hdr->n_entries] - (char *)catpage;

	meta->free_log_head = BufferGetBlockNumber(catbuf);

	MarkBufferDirty(catbuf);
	MarkBufferDirty(metabuf);

	if (RelationNeedsWAL(index))
	{
		XLogRecPtr recptr;

		XLogBeginInsert();
		XLogRegisterBuffer(0, metabuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
		XLogRegisterBuffer(1, catbuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
		recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_FREE_LOG);
		PageSetLSN(catpage, recptr);
		PageSetLSN(metapage, recptr);
	}

	END_CRIT_SECTION();

	(void)new_page;
	UnlockReleaseBuffer(catbuf);
	UnlockReleaseBuffer(metabuf);
	return take;
}

void
pg_tre_free_log_append(Relation index, const BlockNumber *blocks, int n)
{
	uint64		 del_xid_value;
	int			 base = 0;
	BlockNumber *uniq;
	int			 un, w, r;

	if (n <= 0 || blocks == NULL)
		return;

	/*
	 * Defensively dedup the batch before logging.  A block logged twice
	 * would, after the first reclaim frees and FSM-reuses it, let the
	 * second (stale) entry reinitialize a now-live page.  Callers should
	 * already pass distinct blocks (runs do not share pages; coalesced
	 * pages are deduped by pg_tre_run_collect_pages), but a single point
	 * of enforcement here makes the no-double-free invariant local.
	 */
	uniq = palloc((Size)n * sizeof(BlockNumber));
	memcpy(uniq, blocks, (Size)n * sizeof(BlockNumber));
	qsort(uniq, n, sizeof(BlockNumber), blk_cmp);
	for (w = 0, r = 0; r < n; r++)
		if (r == 0 || uniq[r] != uniq[w - 1])
			uniq[w++] = uniq[r];
	un = w;

	/*
	 * One deletion XID for the whole batch.  A scan that could still be
	 * traversing any of these pages began under a snapshot that predates
	 * this XID; once GlobalVisCheckRemovableFullXid passes for it at
	 * drain time, no such scan remains.
	 */
	del_xid_value = ReadNextFullTransactionId().value;

	while (base < un)
	{
		int wrote =
				free_log_append_chunk(index, uniq, base, un, del_xid_value);
		if (wrote <= 0)
			break; /* defensive: avoid an infinite loop */
		base += wrote;
	}

	pfree(uniq);
}

/* --------------------------------------------------------------------
 * Drain
 * -------------------------------------------------------------------- */

/*
 * Reclaim one logged block: reinitialize it to a blank page and remove
 * its entry from the log page, in one WAL'd critical section.  `logbuf`
 * is held EXCLUSIVE; `slot` is the entry index on it.  The entry is
 * removed by swapping in the last entry (order does not matter).
 */
static void
free_log_reclaim_one(
		Relation index, Buffer logbuf, int slot, BlockNumber target)
{
	Page				logpage = BufferGetPage(logbuf);
	PgTreFreeLogHeader *hdr		= free_log_header(logpage);
	PgTreFreeLogEntry  *ents	= free_log_entries(logpage);
	Buffer				tbuf;
	Page				tpage;

	tbuf = ReadBuffer(index, target);
	LockBuffer(tbuf, BUFFER_LOCK_EXCLUSIVE);
	tpage = BufferGetPage(tbuf);

	START_CRIT_SECTION();

	/* Reinitialize the target as a blank page (free-log kind sentinel;
	 * the page is immediately handed to the FSM and reused -- the kind
	 * only matters until pg_tre_extend re-inits it). */
	pg_tre_page_init(tpage, BLCKSZ, PG_TRE_PAGE_FREE_LOG);
	MarkBufferDirty(tbuf);

	/* Remove the entry: move the last one into this slot. */
	ents[slot] = ents[hdr->n_entries - 1];
	hdr->n_entries--;
	((PageHeader)logpage)->pd_lower =
			(char *)&ents[hdr->n_entries] - (char *)logpage;
	MarkBufferDirty(logbuf);

	if (RelationNeedsWAL(index))
	{
		XLogRecPtr recptr;

		XLogBeginInsert();
		XLogRegisterBuffer(0, tbuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
		XLogRegisterBuffer(1, logbuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
		recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_FREE_LOG);
		PageSetLSN(tpage, recptr);
		PageSetLSN(logpage, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(tbuf);

	RecordFreeIndexPage(index, target);
}

BlockNumber
pg_tre_free_log_drain(
		Relation index, Relation heaprel, BlockNumber *out_pending)
{
	PgTreMetaPageData meta;
	BlockNumber		  logblk;
	BlockNumber		  reclaimed = 0;
	BlockNumber		  pending	= 0;
	bool			  any		= false;

	pg_tre_meta_read(index, &meta);
	logblk = meta.free_log_head;

	while (BlockNumberIsValid(logblk))
	{
		Buffer				logbuf;
		Page				logpage;
		PgTreFreeLogHeader *hdr;
		PgTreFreeLogEntry  *ents;
		BlockNumber			next;
		int					slot;

		CHECK_FOR_INTERRUPTS();

		logbuf = pg_tre_read(
				index, logblk, PG_TRE_PAGE_FREE_LOG, BUFFER_LOCK_EXCLUSIVE);
		logpage = BufferGetPage(logbuf);
		hdr		= free_log_header(logpage);
		ents	= free_log_entries(logpage);
		next	= hdr->right_link;

		/*
		 * Reclaim recyclable entries.  free_log_reclaim_one swaps the
		 * last entry into the freed slot, so we re-test the same slot
		 * index after a reclaim rather than advancing.
		 */
		slot = 0;
		while (slot < (int)hdr->n_entries)
		{
			FullTransactionId fxid;
			fxid.value = ents[slot].del_xid_value;

			if (GlobalVisCheckRemovableFullXid(heaprel, fxid))
			{
				BlockNumber target = ents[slot].block;
				free_log_reclaim_one(index, logbuf, slot, target);
				reclaimed++;
				any = true;
				/* hdr->n_entries shrank; ents[slot] is now a fresh entry. */
			}
			else
			{
				pending++;
				slot++;
			}
		}

		UnlockReleaseBuffer(logbuf);
		logblk = next;
	}

	/*
	 * Collapse the log when the head page has fully drained and is the
	 * only page: reset free_log_head to invalid and free the page.
	 * Free-log pages are touched only by VACUUM (under
	 * ShareUpdateExclusiveLock, which excludes other VACUUMs) and never
	 * by a scan, so this needs no XID gate -- nothing else can reference
	 * the page.  The head/meta update is one WAL'd critical section.
	 * Mid-chain empty pages (a newer head still holding un-aged entries
	 * while an older page emptied) are left linked; they are reused by
	 * the next append or collapse once they reach the head, so the chain
	 * stays bounded in the steady state without per-page unlink logic.
	 *
	 * Skipped entirely when there is no free log (the flush_to_run-off
	 * common case), so a VACUUM that never merged a run takes no
	 * exclusive meta lock and is behaviorally a no-op here.
	 */
	if (BlockNumberIsValid(meta.free_log_head))
	{
		Buffer metabuf = pg_tre_read(
				index,
				PG_TRE_META_BLKNO,
				PG_TRE_PAGE_META,
				BUFFER_LOCK_EXCLUSIVE);
		Page		  metapage	= BufferGetPage(metabuf);
		PgTreMetaPage m			= PgTreMetaPageGet(metapage);
		BlockNumber	  head		= m->free_log_head;
		bool		  collapsed = false;

		if (head != 0 && BlockNumberIsValid(head))
		{
			Buffer hbuf = pg_tre_read(
					index, head, PG_TRE_PAGE_FREE_LOG, BUFFER_LOCK_EXCLUSIVE);
			Page				hpage = BufferGetPage(hbuf);
			PgTreFreeLogHeader *hhdr  = free_log_header(hpage);

			if (hhdr->n_entries == 0 && hhdr->right_link == InvalidBlockNumber)
			{
				START_CRIT_SECTION();
				pg_tre_page_init(hpage, BLCKSZ, PG_TRE_PAGE_FREE_LOG);
				m->free_log_head = InvalidBlockNumber;
				MarkBufferDirty(hbuf);
				MarkBufferDirty(metabuf);
				if (RelationNeedsWAL(index))
				{
					XLogRecPtr recptr;
					XLogBeginInsert();
					XLogRegisterBuffer(
							0, metabuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
					XLogRegisterBuffer(
							1, hbuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
					recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_FREE_LOG);
					PageSetLSN(metapage, recptr);
					PageSetLSN(hpage, recptr);
				}
				END_CRIT_SECTION();
				collapsed = true;
				UnlockReleaseBuffer(hbuf);
				RecordFreeIndexPage(index, head);
				any = true;
			}
			else
				UnlockReleaseBuffer(hbuf);
		}
		(void)collapsed;
		UnlockReleaseBuffer(metabuf);
	}

	if (any)
		IndexFreeSpaceMapVacuum(index);

	if (out_pending)
		*out_pending = pending;
	return reclaimed;
}
