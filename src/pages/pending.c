/*
 * src/pages/pending.c - fast-update pending list.
 *
 * Forward-linked chain of pages storing (trigram_hash, tid, position)
 * triples appended by aminsert.  Drained by VACUUM (amvacuumcleanup)
 * or explicit pg_tre_flush(), at which point entries are merged into
 * per-trigram posting trees and the list is truncated.
 *
 * Phase 4 implementation:
 *   - Append path: single writer, exclusive lock on meta + tail page.
 *   - Scan path: share-locks each page in turn, callback per entry.
 *   - Merge path: rebuild the upper tree with merged postings.  This
 *     avoids a real upper_insert (deferred to Phase 8 hardening) at
 *     the cost of temporarily wasting pages that are replaced by the
 *     rebuild; page reuse lands with the Lehman-Yao insert in Phase 8.
 *
 * WAL records emitted:
 *   - XLOG_PTRE_PENDING_INSERT (FPI on the tail page + meta page; when
 *     a full tail page was just linked to a new one, the old tail is
 *     carried as a third FPI block so the next_page link replays)
 *   - XLOG_PTRE_PENDING_MERGE_BEGIN / _COMMIT (FPI framing)
 * Merge-produced posting / upper pages use the Phase 2 INSERT records.
 */

#include "postgres.h"

#include <string.h>

#include "access/xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"
#include "storage/lockdefs.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "pg_tre/buffer.h"
#include "pg_tre/hash.h"
#include "pg_tre/free_log.h"
#include "pg_tre/meta.h"
#include "pg_tre/page.h"
#include "pg_tre/pending.h"
#include "pg_tre/pg_tre.h"
#include "pg_tre/posting.h"
#include "pg_tre/coalesced.h"
#include "pg_tre/run_catalog.h"
#include "pg_tre/sparsemap.h"
#include "pg_tre/upper.h"
#include "pg_tre/xlog.h"

/*
 * Local mirror of upper.c's internal-page entry layout (the struct is
 * file-private to upper.c).  Must match PgTreUpperInternalEntry exactly;
 * mirrored here -- as posting.c does with PgTreVacUpperInternalEntry --
 * so the merge walk can descend a multi-level upper tree.
 */
typedef struct PgTrePendingUpperInternalEntry
{
    uint64      first_key;
    BlockNumber child_blk;
} PgTrePendingUpperInternalEntry;

/* --------------------------------------------------------------------
 * Page layout helpers
 * -------------------------------------------------------------------- */

static inline PgTrePendingHeader *
pending_header(Page page)
{
    return (PgTrePendingHeader *) PageGetContents(page);
}

static inline PgTrePendingEntry *
pending_entries(Page page)
{
    return (PgTrePendingEntry *)
        ((char *) PageGetContents(page) + MAXALIGN(sizeof(PgTrePendingHeader)));
}

static inline int
pending_capacity(void)
{
    Size body = BLCKSZ
              - MAXALIGN(SizeOfPageHeaderData)
              - MAXALIGN(sizeof(PgTrePendingHeader))
              - MAXALIGN(sizeof(PageTreOpaqueData));
    return (int) (body / sizeof(PgTrePendingEntry));
}

static void
pending_page_init(Page page)
{
    PgTrePendingHeader *hdr;

    pg_tre_page_init(page, BLCKSZ, PG_TRE_PAGE_PENDING);
    hdr = pending_header(page);
    hdr->next_page = InvalidBlockNumber;
    hdr->n_entries = 0;
    hdr->used_bytes = 0;

    ((PageHeader) page)->pd_lower =
        (char *) pending_entries(page) - (char *) page;
}

/* --------------------------------------------------------------------
 * Append path
 * -------------------------------------------------------------------- */

/*
 * Acquire a tail page to append into.  Allocates a new page if the
 * current tail is full or the list is empty.  Caller receives an
 * exclusive buffer lock on the returned buffer.  *meta_out is updated
 * in the metapage when a new tail is allocated; the caller is
 * responsible for WAL-logging that update.
 *
 * Returns the tail buffer and fills *meta_buf_out with the still-
 * pinned metapage buffer (also exclusively locked) so the caller can
 * batch a single WAL record covering both pages.
 */
static void
acquire_tail(Relation index, Buffer *meta_buf_out, Buffer *tail_buf_out,
             Buffer *prev_tail_buf_out, bool *tail_is_new)
{
    Buffer   metabuf;
    Page     metapage;
    PgTreMetaPage meta;
    Buffer   tailbuf;

    metabuf = pg_tre_read(index, PG_TRE_META_BLKNO, PG_TRE_PAGE_META,
                          BUFFER_LOCK_EXCLUSIVE);
    metapage = BufferGetPage(metabuf);
    meta = PgTreMetaPageGet(metapage);

    *tail_is_new = false;
    *prev_tail_buf_out = InvalidBuffer;

    if (meta->pending_tail == InvalidBlockNumber)
    {
        tailbuf = pg_tre_extend(index, PG_TRE_PAGE_PENDING);
        pending_page_init(BufferGetPage(tailbuf));

        meta->pending_head = BufferGetBlockNumber(tailbuf);
        meta->pending_tail = meta->pending_head;
        *tail_is_new = true;
    }
    else
    {
        tailbuf = pg_tre_read(index, meta->pending_tail,
                              PG_TRE_PAGE_PENDING, BUFFER_LOCK_EXCLUSIVE);

        if (pending_header(BufferGetPage(tailbuf))->n_entries >=
            pending_capacity())
        {
            Buffer newbuf = pg_tre_extend(index, PG_TRE_PAGE_PENDING);
            pending_page_init(BufferGetPage(newbuf));

            pending_header(BufferGetPage(tailbuf))->next_page =
                BufferGetBlockNumber(newbuf);
            MarkBufferDirty(tailbuf);

            /*
             * Hand the old tail back to the caller *still locked* so the
             * next_page link mutation can be WAL-logged in the same
             * record that initializes the new tail.  Releasing it here
             * (as a prior version did) left the link change un-logged:
             * the append record only covered meta + the new tail, so a
             * standby/crash-recovery replay never learned that the old
             * tail points at the new one, silently breaking the chain
             * and dropping every entry past the old tail on scan.
             */
            *prev_tail_buf_out = tailbuf;

            meta->pending_tail = BufferGetBlockNumber(newbuf);
            tailbuf = newbuf;
            *tail_is_new = true;
        }
    }

    *meta_buf_out = metabuf;
    *tail_buf_out = tailbuf;
}

void
pg_tre_pending_append_batch(Relation index, const uint64 *hashes,
                            const ItemPointerData *tids,
                            const uint32 *positions, int n)
{
    Buffer  metabuf, tailbuf, prev_tailbuf;
    bool    tail_is_new;
    PgTrePendingHeader *hdr;
    PgTrePendingEntry  *entries;
    Page    tailpage, metapage;
    int     i, room;

    if (n <= 0)
        return;
    if (n > PG_TRE_PENDING_BATCH_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("pg_tre: pending batch size %d exceeds max %d",
                        n, PG_TRE_PENDING_BATCH_MAX)));

    while (n > 0)
    {
        acquire_tail(index, &metabuf, &tailbuf, &prev_tailbuf, &tail_is_new);
        tailpage = BufferGetPage(tailbuf);
        metapage = BufferGetPage(metabuf);
        hdr     = pending_header(tailpage);
        entries = pending_entries(tailpage);

        room = pending_capacity() - hdr->n_entries;
        if (room <= 0)
        {
            /* Shouldn't happen: acquire_tail just checked.  Defensive. */
            if (BufferIsValid(prev_tailbuf))
                UnlockReleaseBuffer(prev_tailbuf);
            UnlockReleaseBuffer(tailbuf);
            UnlockReleaseBuffer(metabuf);
            continue;
        }

        int take = (n < room) ? n : room;
        for (i = 0; i < take; i++)
        {
            PgTrePendingEntry *e = &entries[hdr->n_entries + i];
            e->trigram_hash = *hashes++;
            e->tid          = pg_tre_pack_tid((ItemPointer) tids);
            e->position     = *positions++;
            e->_pad         = 0;
            tids++;
        }
        hdr->n_entries  += take;
        hdr->used_bytes  = hdr->n_entries * sizeof(PgTrePendingEntry);
        ((PageHeader) tailpage)->pd_lower =
            (char *) &entries[hdr->n_entries] - (char *) tailpage;

        PgTreMetaPageGet(metapage)->pending_n_entries += take;

        MarkBufferDirty(tailbuf);
        MarkBufferDirty(metabuf);

        if (RelationNeedsWAL(index))
        {
            XLogRecPtr recptr;
            XLogBeginInsert();

            if (tail_is_new)
            {
                /*
                 * Fresh tail page: standby cannot reconstruct the
                 * page-init layout (special-area tag, initial
                 * pd_lower/pd_upper) from a delta, so emit the
                 * full-page image variant.
                 */
                XLogRegisterBuffer(0, metabuf,
                                   REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
                XLogRegisterBuffer(1, tailbuf,
                                   REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
                /*
                 * When the previous tail was full we just linked it to
                 * this new page.  That next_page mutation MUST travel in
                 * the same record, or recovery/standby loses the chain
                 * link.  Ship it as a full-page image (block 2).
                 */
                if (BufferIsValid(prev_tailbuf))
                    XLogRegisterBuffer(2, prev_tailbuf,
                                       REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
                recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_PENDING_INSERT);
            }
            else
            {
                /*
                 * Existing tail page: emit a delta describing exactly
                 * the bytes that changed.  Cuts WAL volume from two
                 * full-page images (~16 KB) to a few hundred bytes.
                 *
                 * The standby's redo applies the delta on top of its
                 * current page state via XLogReadBufferForRedo.  See
                 * pg_tre_redo_pending_insert_delta in src/wal/xlog.c.
                 */
                xl_pg_tre_pending_insert_meta_delta meta_d;
                xl_pg_tre_pending_insert_tail_delta tail_d;

                meta_d.n_entries_added = (uint32) take;

                tail_d.prev_n_entries = (uint16)
                    (hdr->n_entries - take);
                tail_d.take = (uint16) take;

                XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);
                XLogRegisterBufData(0, (char *) &meta_d, sizeof(meta_d));

                XLogRegisterBuffer(1, tailbuf, REGBUF_STANDARD);
                XLogRegisterBufData(1, (char *) &tail_d, sizeof(tail_d));
                XLogRegisterBufData(1,
                    (char *) &entries[tail_d.prev_n_entries],
                    take * sizeof(PgTrePendingEntry));

                recptr = XLogInsert(RM_PG_TRE_ID,
                                    XLOG_PTRE_PENDING_INSERT |
                                    XLOG_PTRE_DELTA_FLAG);
            }
            PageSetLSN(tailpage, recptr);
            PageSetLSN(metapage, recptr);
            if (BufferIsValid(prev_tailbuf))
                PageSetLSN(BufferGetPage(prev_tailbuf), recptr);
        }

        if (BufferIsValid(prev_tailbuf))
            UnlockReleaseBuffer(prev_tailbuf);
        UnlockReleaseBuffer(tailbuf);
        UnlockReleaseBuffer(metabuf);

        hashes    += 0;    /* hashes already advanced above */
        positions += 0;    /* positions already advanced above */
        n         -= take;
    }
}

void
pg_tre_pending_append(Relation index, uint64 trigram_hash,
                      ItemPointer tid, uint32 position)
{
    ItemPointerData one_tid = *tid;
    pg_tre_pending_append_batch(index, &trigram_hash, &one_tid, &position, 1);
}

/* --------------------------------------------------------------------
 * Scan path (union into live queries + merge ingest)
 * -------------------------------------------------------------------- */

void
pg_tre_pending_scan(Relation index, PgTrePendingCallback callback, void *ctx)
{
    PgTreMetaPageData meta;
    BlockNumber blk;

    pg_tre_meta_read(index, &meta);
    blk = meta.pending_head;

    while (blk != InvalidBlockNumber)
    {
        Buffer  buf = pg_tre_read(index, blk, PG_TRE_PAGE_PENDING,
                                  BUFFER_LOCK_SHARE);
        Page    page = BufferGetPage(buf);
        PgTrePendingHeader *hdr = pending_header(page);
        PgTrePendingEntry  *ent = pending_entries(page);
        BlockNumber next = hdr->next_page;
        int n = hdr->n_entries;
        int i;

        for (i = 0; i < n; i++)
        {
            ItemPointerData tid;
            pg_tre_unpack_tid(ent[i].tid, &tid);
            callback(ent[i].trigram_hash, &tid, ent[i].position, ctx);
        }

        UnlockReleaseBuffer(buf);
        blk = next;
    }
}

/* --------------------------------------------------------------------
 * Watermark-bounded scan (merge ingest)
 * --------------------------------------------------------------------
 *
 * A merge must consume only the entries that existed in the pending
 * list when the merge began, and truncate exactly that prefix -- no
 * more, no less.  Anything a concurrent aminsert appends while the
 * (long-running) merge rebuilds the upper tree must survive into the
 * next merge.
 *
 * The append path only ever *grows* the list: it appends entries to
 * the tail page, or links a fresh tail page off the old tail's
 * next_page and advances meta->pending_tail.  The head never moves
 * except when the list is finalized (here).  So a stable "high water
 * mark" of the consumed region is fully described by:
 *
 *     (watermark_tail, watermark_tail_n)
 *
 * meaning "consume every entry on every page from pending_head up to
 * and including watermark_tail, but on watermark_tail only the first
 * watermark_tail_n entries."  Entries at index >= watermark_tail_n on
 * watermark_tail, and any entries on pages linked after it, are
 * concurrent appends that are left untouched.
 *
 * pg_tre_pending_scan_watermark captures the watermark from the meta +
 * tail page under share locks at scan start, then walks the chain
 * stopping at the watermark.  Because the scan is lock-coupled per
 * page with the exclusive-locked append path, and the head/tail are
 * read from the meta under its buffer lock, the captured watermark is
 * a consistent snapshot of a prefix that was fully present.
 */
static void
pg_tre_pending_scan_watermark(Relation index, PgTrePendingCallback callback,
                              void *ctx, BlockNumber *watermark_tail_out,
                              uint32 *watermark_tail_n_out,
                              BlockNumber *head_out)
{
    PgTreMetaPageData meta;
    BlockNumber blk;
    BlockNumber wm_tail;
    uint32      wm_tail_n = 0;

    pg_tre_meta_read(index, &meta);
    *head_out = meta.pending_head;
    wm_tail = meta.pending_tail;

    if (meta.pending_head == InvalidBlockNumber)
    {
        *watermark_tail_out   = InvalidBlockNumber;
        *watermark_tail_n_out = 0;
        return;
    }

    /*
     * Snapshot the number of entries on the watermark tail page now,
     * under its share lock.  Any append that lands after this point
     * either bumps n_entries on this same page (entries we will not
     * consume) or links a new tail past it (pages we will not visit),
     * so both survive the truncate.
     */
    {
        Buffer  tbuf = pg_tre_read(index, wm_tail, PG_TRE_PAGE_PENDING,
                                   BUFFER_LOCK_SHARE);
        wm_tail_n = pending_header(BufferGetPage(tbuf))->n_entries;
        UnlockReleaseBuffer(tbuf);
    }

    blk = meta.pending_head;
    while (blk != InvalidBlockNumber)
    {
        Buffer  buf = pg_tre_read(index, blk, PG_TRE_PAGE_PENDING,
                                  BUFFER_LOCK_SHARE);
        Page    page = BufferGetPage(buf);
        PgTrePendingHeader *hdr = pending_header(page);
        PgTrePendingEntry  *ent = pending_entries(page);
        BlockNumber next = hdr->next_page;
        int n = hdr->n_entries;
        int i;

        /* On the watermark tail page, consume only the snapshotted prefix. */
        if (blk == wm_tail && (uint32) n > wm_tail_n)
            n = (int) wm_tail_n;

        for (i = 0; i < n; i++)
        {
            ItemPointerData tid;
            pg_tre_unpack_tid(ent[i].tid, &tid);
            callback(ent[i].trigram_hash, &tid, ent[i].position, ctx);
        }

        UnlockReleaseBuffer(buf);

        /* Stop at the watermark tail; pages past it are concurrent appends. */
        if (blk == wm_tail)
            break;
        blk = next;
    }

    *watermark_tail_out   = wm_tail;
    *watermark_tail_n_out = wm_tail_n;
}

/* --------------------------------------------------------------------
 * Merge path
 * --------------------------------------------------------------------
 *
 * Strategy: collect pending-list entries into an in-memory hash of
 * (trigram_hash -> TID set), compute the union with each existing
 * posting, and rebuild the upper tree from scratch using the merged
 * postings plus any existing postings that weren't touched.
 */

typedef struct MergeEntry
{
    uint64       trigram_hash;
    /*
     * Phase 4.1: collect TIDs into a palloc'd dynamic array instead of a
     * dynamically-grown malloc-backed sparsemap.  The sparsemap dynamic
     * resize path triggered heap corruption on larger fixtures; palloc/
     * repalloc is rock-solid.  The array is converted to a sparsemap
     * once at materialize time when we know the final cardinality.
     */
    uint64      *tids;           /* palloc'd in mc->mcxt */
    int          n_tids;
    int          tids_alloced;
    bool         seen_in_upper;  /* set during upper walk */
    BlockNumber  merged_root;    /* posting root after merge */
    uint8       *merged_inline;  /* inline blob after merge (palloc) */
    Size         merged_inline_bytes;
} MergeEntry;

typedef struct MergeCtx
{
    MergeEntry **bucket_tab;     /* open addressing hash table */
    int          bucket_cap;
    int          n_entries;
    MemoryContext mcxt;
    bool         pending_only;   /* B1.3 flush-to-run: do NOT union with
                                  * the existing posting -- the new run
                                  * holds only the pending TIDs */
} MergeCtx;

static int
hash_bucket(uint64 h, int cap)
{
    return (int) (h % (uint64) cap);
}

static MergeEntry *
merge_find(MergeCtx *mc, uint64 h, bool create)
{
    int idx = hash_bucket(h, mc->bucket_cap);
    int probes = 0;

    while (mc->bucket_tab[idx] != NULL)
    {
        if (mc->bucket_tab[idx]->trigram_hash == h)
            return mc->bucket_tab[idx];
        idx = (idx + 1) % mc->bucket_cap;
        if (++probes > mc->bucket_cap)
            elog(ERROR, "pg_tre: merge hash table full");
    }

    if (!create)
        return NULL;

    /* Rehash if load factor exceeded. */
    if (mc->n_entries * 2 >= mc->bucket_cap)
    {
        int old_cap = mc->bucket_cap;
        MergeEntry **old_tab = mc->bucket_tab;
        int i;

        mc->bucket_cap = old_cap * 2;
        mc->bucket_tab = MemoryContextAllocZero(mc->mcxt,
                            mc->bucket_cap * sizeof(MergeEntry *));

        for (i = 0; i < old_cap; i++)
        {
            if (old_tab[i] == NULL)
                continue;
            int j = hash_bucket(old_tab[i]->trigram_hash, mc->bucket_cap);
            while (mc->bucket_tab[j] != NULL)
                j = (j + 1) % mc->bucket_cap;
            mc->bucket_tab[j] = old_tab[i];
        }
        pfree(old_tab);

        return merge_find(mc, h, create);
    }

    {
        MergeEntry *e = MemoryContextAllocZero(mc->mcxt, sizeof(*e));
        e->trigram_hash        = h;
        e->tids                = MemoryContextAlloc(mc->mcxt,
                                       16 * sizeof(uint64));
        e->n_tids              = 0;
        e->tids_alloced        = 16;
        e->merged_root         = InvalidBlockNumber;
        e->merged_inline       = NULL;
        e->merged_inline_bytes = 0;
        e->seen_in_upper       = false;

        mc->bucket_tab[idx] = e;
        mc->n_entries++;
        return e;
    }
}

/*
 * Collect pending-list entries into the merge hash.
 */
static void
collect_pending_cb(uint64 hash, ItemPointer tid, uint32 position, void *ctx)
{
    MergeCtx *mc = (MergeCtx *) ctx;
    MergeEntry *e;
    uint64 packed;

    (void) position;     /* Phase 4 ignores positions; Phase 5 uses them */
    e = merge_find(mc, hash, true);
    packed = pg_tre_pack_tid(tid);

    if (e->n_tids >= e->tids_alloced)
    {
        MemoryContext old = MemoryContextSwitchTo(mc->mcxt);
        e->tids_alloced *= 2;
        e->tids = repalloc(e->tids, e->tids_alloced * sizeof(uint64));
        MemoryContextSwitchTo(old);
    }
    e->tids[e->n_tids++] = packed;
}

/*
 * Iterator over merged postings for upper-tree bulkload.  Walks both
 * the existing upper tree AND the hash of pending-merged entries,
 * yielding them in sorted (trigram_hash) order.  For each trigram:
 *   - if new in pending and absent in upper: union new only
 *   - if in both: union existing + new
 *   - if only in upper: pass through unchanged
 */
typedef struct UpperRebuildIter
{
    MergeEntry **sorted;
    int          n_sorted;
    int          i_sorted;

    /* For upper-tree walk (existing entries): */
    Relation     index;
    Buffer       upper_buf;      /* pinned + share-locked */
    int          upper_idx;
    int          upper_n;
    PgTreUpperLeafEntry *upper_entries;

    MemoryContext mcxt;
    MergeCtx    *mc;
} UpperRebuildIter;

static int
cmp_merge_hash(const void *a, const void *b)
{
    MergeEntry * const *ea = a;
    MergeEntry * const *eb = b;
    if ((*ea)->trigram_hash < (*eb)->trigram_hash) return -1;
    if ((*ea)->trigram_hash > (*eb)->trigram_hash) return 1;
    return 0;
}

/* Pre-materialize the "merged posting" for each MergeEntry: union
 * existing posting with new_tids, serialize.  Called once before
 * rebuild iteration starts. */
static void
materialize_merged_postings(Relation index, MergeCtx *mc)
{
    int i;
    for (i = 0; i < mc->bucket_cap; i++)
    {
        MergeEntry *e = mc->bucket_tab[i];
        PgTreUpperRef ref;
        sm_t *existing;
        sm_t *merged;
        int k;
        size_t sz;
        uint8 *out_buf;
        sm_t *fresh;

        if (e == NULL)
            continue;

        /*
         * Convert our collected uint64 TID array to a malloc-backed
         * sparsemap.  We start with a generous initial capacity
         * (16 B/TID + 1 KiB overhead) and use sm_add_grow() so the
         * buffer doubles geometrically on ENOSPC — sparse / scattered
         * TID streams can incur a fresh chunk header per TID, blowing
         * past any static estimate.
         */
        {
            size_t cap = (size_t) e->n_tids * 16 + 1024;
            sm_t *fresh_acc = sm_create(cap);
            if (fresh_acc == NULL)
                ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
                    errmsg("pg_tre: merge accumulator allocation failed")));
            for (k = 0; k < e->n_tids; k++)
            {
                int retries = 0;
                while (sm_add_grow(&fresh_acc, e->tids[k]) == SM_IDX_MAX)
                {
                    if (++retries > 16)
                    {
                        size_t final_cap = sm_get_capacity(fresh_acc);
                        sm_free(fresh_acc);
                        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                            errmsg("pg_tre: merge accumulator sm_add_grow exhausted retries (k=%d, n_tids=%d, cap=%zu)",
                                   k, e->n_tids, final_cap)));
                    }
                }
            }
            merged = fresh_acc;
        }

        /*
         * Union with the existing posting (if any).  pg_tre_posting_materialize
         * returns a palloc-backed sm_t wrap; sm_union returns a
         * new malloc-backed sparsemap.
         */
        existing = NULL;
        if (!mc->pending_only &&
            pg_tre_upper_lookup(index, e->trigram_hash, &ref))
        {
            existing = pg_tre_posting_materialize(index, ref.root,
                                                  ref.inline_data,
                                                  ref.inline_bytes);
            pg_tre_upper_release(&ref);
            e->seen_in_upper = true;
        }

        if (existing != NULL)
        {
            sm_t *u = sm_union(existing, merged);
            free(existing);
            free(merged);
            merged = u;
        }

        /*
         * Rather than manually re-serializing, just copy the merged
         * sparsemap. The sparsemap library handles the serialization
         * internally, avoiding the capacity estimation issues that
         * led to heap corruption with wrapped maps.
         */
        fresh = sm_copy(merged);
        if (fresh == NULL)
            ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
                errmsg("pg_tre: out of memory copying sparsemap")));

        sz = sm_get_size(fresh);

        /* Copy the serialized data to a palloc'd buffer. */
        out_buf = MemoryContextAlloc(mc->mcxt, sz);
        memcpy(out_buf, sm_get_data(fresh), sz);
        free(fresh);

        /* Decide inline vs on-disk leaf. */
        if (sz <= PG_TRE_INLINE_POSTING_MAX)
        {
            e->merged_inline       = out_buf;
            e->merged_inline_bytes = sz;
            e->merged_root         = InvalidBlockNumber;
        }
        else
        {
            PgTrePostingBuilder *b;
            uint64 j;
            /* Pre-size: worst case each TID is ~8 bytes in sparsemap,
             * plus overhead.  Size for 16 bytes per TID is generous. */
            size_t expected = (size_t) e->n_tids * 16 + 1024;
            b = pg_tre_posting_build_begin_sized(index, e->trigram_hash,
                                                  false, expected);
            /*
             * Iterate the sparsemap's MEMBERS in rank order via
             * sm_next_member() — O(cardinality), with a cursor fast-path
             * for sequential iteration — rather than probing every integer
             * in [min_idx, max_idx] with sm_contains().  The old range
             * scan was O(TID-range): for a trigram whose TIDs span a wide
             * block range it spins ~100% CPU for minutes even though the
             * set may hold only a handful of members.
             */
            for (j = sm_next_member(merged, SM_IDX_MAX);
                 j != SM_IDX_MAX;
                 j = sm_next_member(merged, j))
            {
                ItemPointerData tid;
                CHECK_FOR_INTERRUPTS();
                pg_tre_unpack_tid(j, &tid);
                pg_tre_posting_build_add(b, &tid, NULL, 0, NULL);
            }
            {
                const uint8 *id;
                Size ib;
                e->merged_root = pg_tre_posting_build_finish(b, &id, &ib);
                if (e->merged_root == InvalidBlockNumber)
                {
                    e->merged_inline = (uint8 *) id;
                    e->merged_inline_bytes = ib;
                }
            }
            pg_tre_posting_build_free(b);
            pfree(out_buf);
        }

        free(merged);
    }
}

/* Iterator that produces sorted (hash, root_or_inline) entries merging
 * the existing upper-tree leaf with MergeEntry overrides. */
typedef struct RebuildState
{
    MergeEntry **sorted;
    int          n_sorted;
    int          i_sorted;

    /* Snapshot of existing upper-tree leaf entries (palloc'd copy). */
    PgTreUpperLeafEntry *existing;
    int          n_existing;
    int          i_existing;

    /* When we pass through an existing entry, we need the inline bytes
     * to remain valid after the upper rebuild has released its buffer.
     * So materialize entries to palloc'd copies up front. */
    uint8      **existing_inline;
    int          existing_cap;    /* allocated capacity of the two arrays */
} RebuildState;

static bool
rebuild_iter(void *ctx, uint64 *hash, BlockNumber *root,
             const uint8 **inline_data, Size *inline_bytes)
{
    RebuildState *r = ctx;

    while (true)
    {
        MergeEntry            *m = (r->i_sorted   < r->n_sorted)
                                      ? r->sorted[r->i_sorted] : NULL;
        PgTreUpperLeafEntry   *e = (r->i_existing < r->n_existing)
                                      ? &r->existing[r->i_existing] : NULL;

        if (m == NULL && e == NULL)
            return false;

        /* Choose the smallest hash from the two streams. */
        if (m == NULL ||
            (e != NULL && e->trigram_hash < m->trigram_hash))
        {
            *hash         = e->trigram_hash;
            *root         = e->posting_root;
            *inline_data  = r->existing_inline[r->i_existing];
            *inline_bytes = e->inline_bytes;
            r->i_existing++;
            return true;
        }

        if (e == NULL || m->trigram_hash < e->trigram_hash)
        {
            *hash         = m->trigram_hash;
            *root         = m->merged_root;
            *inline_data  = m->merged_inline;
            *inline_bytes = m->merged_inline_bytes;
            r->i_sorted++;
            return true;
        }

        /* Equal: emit merged (which already includes existing). */
        *hash         = m->trigram_hash;
        *root         = m->merged_root;
        *inline_data  = m->merged_inline;
        *inline_bytes = m->merged_inline_bytes;
        r->i_sorted++;
        r->i_existing++;
        return true;
    }
}

/*
 * Append all PgTreUpperLeafEntry rows of one upper LEAF page to the
 * RebuildState's growable existing/existing_inline arrays, copying any
 * inline blobs into mcxt-owned storage.  `buf` must hold a SHARE lock
 * on a PG_TRE_PAGE_UPPER_L page; it is NOT released here.
 */
static void
snapshot_upper_leaf(RebuildState *r, MemoryContext mcxt, Buffer buf)
{
    Page                 page = BufferGetPage(buf);
    PgTreUpperLeafEntry *src  = (PgTreUpperLeafEntry *) PageGetContents(page);
    int                  n    = PageTreGetOpaque(page)->flags;
    int                  i;
    Size                 inline_offset;
    const uint8         *blob_base;
    int                  need;

    if (n == 0)
    {
        /* Fallback: old-format page without the n_entries counter. */
        n = (((PageHeader) page)->pd_lower - sizeof(PageHeaderData))
            / sizeof(PgTreUpperLeafEntry);
    }
    if (n <= 0)
        return;

    /* Grow the two parallel arrays to fit n more entries. */
    need = r->n_existing + n;
    if (need > r->existing_cap)
    {
        int newcap = (r->existing_cap == 0) ? need : r->existing_cap * 2;
        if (newcap < need)
            newcap = need;
        if (r->existing == NULL)
        {
            r->existing = MemoryContextAlloc(mcxt,
                            (Size) newcap * sizeof(PgTreUpperLeafEntry));
            r->existing_inline = MemoryContextAllocZero(mcxt,
                            (Size) newcap * sizeof(uint8 *));
        }
        else
        {
            r->existing = repalloc(r->existing,
                            (Size) newcap * sizeof(PgTreUpperLeafEntry));
            r->existing_inline = repalloc(r->existing_inline,
                            (Size) newcap * sizeof(uint8 *));
            /* Zero the freshly-grown inline slots. */
            memset(r->existing_inline + r->existing_cap, 0,
                   (Size) (newcap - r->existing_cap) * sizeof(uint8 *));
        }
        r->existing_cap = newcap;
    }

    /*
     * Inline blobs are concatenated after the entry array, in entry
     * order, without per-entry offsets (see pg_tre_upper_lookup).  We
     * already hold a SHARE lock on this leaf page, so we copy the blob
     * straight out of the page we hold, tracking the running offset.
     */
    inline_offset = 0;
    blob_base     = (const uint8 *) &src[n];
    for (i = 0; i < n; i++)
    {
        int dst = r->n_existing + i;
        r->existing[dst] = src[i];
        if ((src[i].inline_bytes & PG_TRE_COALESCED_FLAG) != 0)
        {
            /*
             * Coalesced entry: inline_bytes is a (flag | slot) marker,
             * not a blob length, and the blob lives on a separate
             * coalesced page (not in this leaf).  Preserve the entry
             * (posting_root = coalesced block, inline_bytes = marker)
             * verbatim and do NOT consume from blob_base.  The merge's
             * collapse_fold_run resolves it via pg_tre_posting_
             * materialize, which honors the coalesced flag.
             */
            r->existing_inline[dst] = NULL;
        }
        else if (src[i].inline_bytes > 0)
        {
            uint8 *copy = MemoryContextAlloc(mcxt, src[i].inline_bytes);
            memcpy(copy, blob_base + inline_offset, src[i].inline_bytes);
            r->existing_inline[dst]       = copy;
            r->existing[dst].inline_bytes = src[i].inline_bytes;
            inline_offset += src[i].inline_bytes;
        }
        else
        {
            r->existing_inline[dst] = NULL;
        }
    }
    r->n_existing += n;
}

/*
 * Recursively walk the upper tree rooted at `blk`, snapshotting every
 * leaf's entries in left-to-right (ascending-key) order.  Upper leaves
 * are NOT right-linked -- they are addressed only through internal
 * pages -- so a full recursive descent is required.  Internal pages
 * store children in ascending first_key order, so visiting children in
 * array order yields globally sorted leaf entries, which rebuild_iter
 * requires.
 */
static void
snapshot_upper_subtree(Relation index, RebuildState *r, MemoryContext mcxt,
                       BlockNumber blk)
{
    Buffer        buf;
    Page          page;
    PageTreOpaque opq;

    CHECK_FOR_INTERRUPTS();

    buf  = pg_tre_read(index, blk, PG_TRE_PAGE_INVALID, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);
    opq  = PageTreGetOpaque(page);

    if (opq->page_kind == PG_TRE_PAGE_UPPER_L)
    {
        snapshot_upper_leaf(r, mcxt, buf);
        UnlockReleaseBuffer(buf);
        return;
    }

    if (opq->page_kind != PG_TRE_PAGE_UPPER)
    {
        UnlockReleaseBuffer(buf);
        elog(ERROR,
             "pg_tre: unexpected page kind %u during upper-tree merge walk",
             opq->page_kind);
    }

    /*
     * Internal page: collect child block numbers under the lock, then
     * release before recursing so we never hold more than one upper
     * buffer locked at a time (recursion re-locks each child).
     */
    {
        PgTrePendingUpperInternalEntry *ents =
            (PgTrePendingUpperInternalEntry *) PageGetContents(page);
        int n = (((PageHeader) page)->pd_lower - sizeof(PageHeaderData))
                / sizeof(PgTrePendingUpperInternalEntry);
        BlockNumber *children;
        int i;

        if (n <= 0)
        {
            UnlockReleaseBuffer(buf);
            return;
        }

        children = MemoryContextAlloc(mcxt, (Size) n * sizeof(BlockNumber));
        for (i = 0; i < n; i++)
            children[i] = ents[i].child_blk;
        UnlockReleaseBuffer(buf);

        for (i = 0; i < n; i++)
            snapshot_upper_subtree(index, r, mcxt, children[i]);

        pfree(children);
    }
}

/* Snapshot the existing upper-tree leaf entries into palloc'd storage
 * so we can release the buffers before rebuilding.  Handles both a
 * single-leaf root and a multi-level internal tree. */
static void
snapshot_existing_upper(Relation index, RebuildState *r,
                        MemoryContext mcxt)
{
    PgTreMetaPageData meta;

    r->existing        = NULL;
    r->existing_inline = NULL;
    r->n_existing      = 0;
    r->i_existing      = 0;
    r->existing_cap    = 0;

    pg_tre_meta_read(index, &meta);
    if (meta.root_upper == InvalidBlockNumber)
        return;

    snapshot_upper_subtree(index, r, mcxt, meta.root_upper);
}

/*
 * Finalize a merge atomically: swap in the new upper-tree root AND
 * truncate the consumed prefix of the pending list in a SINGLE meta
 * page write / SINGLE WAL record, inside one critical section.
 *
 * Prior to this the root swap (pg_tre_meta_set_roots) and the list
 * truncate were two independent WAL records.  A crash between them
 * left the index with the merged roots but a non-truncated pending
 * list, so the next merge re-consumed (double-counted) the same
 * entries.  Folding both mutations into the meta page under one
 * record makes replay all-or-nothing: recovery either sees the
 * pre-merge state (old root + full list) or the post-merge state
 * (new root + rewound list), never a torn mix.
 *
 * Truncation is bounded by the watermark captured at scan start
 * (watermark_tail / watermark_tail_n):
 *
 *   - If the consumed prefix is the entire list (the watermark tail
 *     is still the meta tail and held no entries past the watermark,
 *     and no newer page was linked), reset head=tail=invalid.
 *
 *   - Otherwise concurrent inserts appended after the scan.  The
 *     surviving entries -- those at index >= watermark_tail_n on the
 *     watermark tail page, plus every page linked after it -- must be
 *     preserved.  We compact the watermark tail page in place,
 *     shifting the survivors down to slot 0, and re-point
 *     pending_head at the watermark tail.  Pages strictly before the
 *     watermark tail are unlinked (orphaned until REINDEX, matching
 *     the pre-existing reclaim policy).
 *
 * pending_n_entries is recomputed from the surviving chain so the
 * counter stays exact.  The number of entries actually consumed is
 * returned via *consumed_out.
 *
 * Caller passes the new upper-tree root and the n_trigrams /
 * n_tuples_indexed stats that pg_tre_meta_set_roots would have set.
 */
static void
finalize_merge(Relation index, BlockNumber new_root, BlockNumber root_range,
               uint64 n_trigrams, uint64 n_tuples_indexed,
               BlockNumber watermark_tail, uint32 watermark_tail_n,
               uint64 *consumed_out)
{
    Buffer  metabuf;
    Page    metapage;
    PgTreMetaPage m;
    Buffer  tailbuf = InvalidBuffer;
    Page    tailpage = NULL;
    PgTrePendingHeader *thdr = NULL;
    bool    rewind_tail = false;
    uint64  consumed = 0;
    uint32  survive = 0;
    uint64  surviving_total = 0;

    metabuf = pg_tre_read(index, PG_TRE_META_BLKNO, PG_TRE_PAGE_META,
                          BUFFER_LOCK_EXCLUSIVE);
    metapage = BufferGetPage(metabuf);
    m = PgTreMetaPageGet(metapage);

    /*
     * Re-examine the watermark tail under the meta exclusive lock.  If
     * it now holds more entries than the watermark, or it is no longer
     * the list tail, concurrent inserts ran and we must preserve the
     * survivors rather than blow the whole list away.
     */
    if (watermark_tail != InvalidBlockNumber)
    {
        tailbuf = pg_tre_read(index, watermark_tail, PG_TRE_PAGE_PENDING,
                              BUFFER_LOCK_EXCLUSIVE);
        tailpage = BufferGetPage(tailbuf);
        thdr = pending_header(tailpage);

        if ((uint32) thdr->n_entries > watermark_tail_n ||
            thdr->next_page != InvalidBlockNumber ||
            m->pending_tail != watermark_tail)
            rewind_tail = true;
    }

    /*
     * When concurrent appends survived, tally them up FIRST -- outside
     * any critical section, since walking the trailing chain acquires
     * buffer locks (which must never happen inside a crit section).
     * The append path always takes the meta lock before any tail page,
     * so taking these share locks while we already hold the exclusive
     * meta + watermark-tail locks introduces no lock-order inversion.
     */
    if (rewind_tail)
    {
        BlockNumber b;

        survive = (uint32) thdr->n_entries - watermark_tail_n;
        surviving_total = survive;

        b = thdr->next_page;
        while (b != InvalidBlockNumber)
        {
            Buffer  cb = pg_tre_read(index, b, PG_TRE_PAGE_PENDING,
                                     BUFFER_LOCK_SHARE);
            PgTrePendingHeader *chdr = pending_header(BufferGetPage(cb));
            surviving_total += chdr->n_entries;
            b = chdr->next_page;
            UnlockReleaseBuffer(cb);
        }
    }

    START_CRIT_SECTION();

    if (!rewind_tail)
    {
        /* Entire list consumed. */
        consumed = m->pending_n_entries;
        m->pending_head      = InvalidBlockNumber;
        m->pending_tail      = InvalidBlockNumber;
        m->pending_n_entries = 0;
    }
    else
    {
        /*
         * Compact survivors on the watermark tail down to slot 0 and
         * make it the new head.  Pages before it are orphaned (the
         * pre-existing "rely on REINDEX to reclaim" policy).
         */
        PgTrePendingEntry *tent = pending_entries(tailpage);
        uint64 old_total = m->pending_n_entries;

        if (watermark_tail_n > 0 && survive > 0)
            memmove(&tent[0], &tent[watermark_tail_n],
                    (size_t) survive * sizeof(PgTrePendingEntry));

        thdr->n_entries  = survive;
        thdr->used_bytes = survive * sizeof(PgTrePendingEntry);
        ((PageHeader) tailpage)->pd_lower =
            (char *) &tent[survive] - (char *) tailpage;

        m->pending_head = watermark_tail;
        /* pending_tail unchanged (still the real tail). */

        consumed = (old_total >= surviving_total)
                   ? old_total - surviving_total : 0;
        m->pending_n_entries = surviving_total;

        MarkBufferDirty(tailbuf);
    }

    /* Swap in the new tree roots / stats in the SAME meta write. */
    m->root_upper       = new_root;
    m->root_range       = root_range;
    m->n_trigrams       = n_trigrams;
    m->n_tuples_indexed = n_tuples_indexed;

    MarkBufferDirty(metabuf);

    if (RelationNeedsWAL(index))
    {
        XLogRecPtr recptr;
        XLogBeginInsert();
        /*
         * FPI both pages.  Bundling the meta (root swap + list head/
         * counter) and the rewound watermark tail in one record makes
         * replay atomic: a crash can never reproduce "new root, stale
         * list" or a half-compacted tail.
         */
        XLogRegisterBuffer(0, metabuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        if (rewind_tail)
            XLogRegisterBuffer(1, tailbuf,
                               REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
        recptr = XLogInsert(RM_PG_TRE_ID, XLOG_PTRE_PENDING_MERGE_C);
        PageSetLSN(metapage, recptr);
        if (rewind_tail)
            PageSetLSN(tailpage, recptr);
    }

    END_CRIT_SECTION();

    if (BufferIsValid(tailbuf))
        UnlockReleaseBuffer(tailbuf);
    UnlockReleaseBuffer(metabuf);

    if (consumed_out != NULL)
        *consumed_out = consumed;
}

uint64
pg_tre_pending_merge(Relation index)
{
    PgTreMetaPageData meta;
    MemoryContext     merge_cxt, old;
    MergeCtx          mc;
    RebuildState      rs;
    int               i, k;
    BlockNumber       new_root;
    uint64            n_merged;
    BlockNumber       wm_tail;
    uint32            wm_tail_n;
    BlockNumber       wm_head;

    pg_tre_meta_read(index, &meta);
    if (meta.pending_head == InvalidBlockNumber)
        return 0;

    merge_cxt = AllocSetContextCreate(CurrentMemoryContext,
                                      "pg_tre merge", ALLOCSET_DEFAULT_SIZES);
    old = MemoryContextSwitchTo(merge_cxt);

    memset(&mc, 0, sizeof(mc));
    mc.bucket_cap = 256;
    mc.bucket_tab = MemoryContextAllocZero(merge_cxt,
                       mc.bucket_cap * sizeof(MergeEntry *));
    mc.mcxt       = merge_cxt;

    /*
     * Consume only the prefix that existed when the merge began,
     * capturing the watermark so the truncate later removes exactly
     * that prefix.  Entries a concurrent aminsert appends during the
     * (long) rebuild below land past the watermark and survive into
     * the next merge instead of being silently dropped.
     */
    pg_tre_pending_scan_watermark(index, collect_pending_cb, &mc,
                                  &wm_tail, &wm_tail_n, &wm_head);

    mc.pending_only = pg_tre_flush_to_run;
    materialize_merged_postings(index, &mc);

    /* Build a sorted array of MergeEntry pointers. */
    rs.sorted  = MemoryContextAlloc(merge_cxt,
                    mc.n_entries * sizeof(MergeEntry *));
    k = 0;
    for (i = 0; i < mc.bucket_cap; i++)
        if (mc.bucket_tab[i] != NULL)
            rs.sorted[k++] = mc.bucket_tab[i];
    Assert(k == mc.n_entries);
    qsort(rs.sorted, k, sizeof(MergeEntry *), cmp_merge_hash);
    rs.n_sorted  = k;
    rs.i_sorted  = 0;

    if (pg_tre_flush_to_run && k > 0)
    {
        /*
         * Phase B1.3 flush-to-run: build a pending-ONLY run (its own
         * upper tree over just the new TIDs) and append it to the run
         * catalog, leaving the base run untouched.  The multi-run scan
         * unions all runs at query time.  Order matters for crash
         * safety: append the run (crash-safe, WAL'd) FIRST, then
         * truncate the consumed pending prefix.  A crash between leaves
         * the pending list intact -> the next merge re-flushes it into
         * a second run with the same TIDs, which the scan's newest-wins
         * union dedups -- wasteful but never wrong, and self-heals.
         */
        BlockNumber run_root;
        PgTreRun    run;
        uint64      consumed = 0;
        uint64      run_n_tuples = 0;
        uint64      lo_hash = rs.sorted[0]->trigram_hash;
        uint64      hi_hash = rs.sorted[k - 1]->trigram_hash;

        rs.existing = NULL;
        rs.existing_inline = NULL;
        rs.n_existing = 0;
        rs.i_existing = 0;
        rs.existing_cap = 0;

        run_root = pg_tre_upper_bulkload(index, rebuild_iter, &rs);

        /*
         * Faithful-enough per-run accounting (introspection via
         * tre_run_catalog_status).  n_trigrams is the distinct-trigram
         * count in this flush (k).  n_tuples is the distinct heap-TID
         * count the run covers; computing it exactly here meant building
         * a second full sparsemap over EVERY (trigram, TID) pair --
         * millions of sm_add_grow calls per flush, a large slice of the
         * 50 s/flush cost the field report measured.  A heap TID recurs
         * across ~5 trigrams, so estimate distinct TIDs as
         * (sum of per-trigram cardinalities) / 5.  This is a cosmetic
         * stat (planner selectivity / introspection); the estimate is
         * the same approximation the non-run merge path already uses for
         * n_tuples_indexed, and never affects correctness or atomicity.
         */
        {
            uint64 total_postings = 0;
            int    si;
            for (si = 0; si < k; si++)
                total_postings += (uint64) rs.sorted[si]->n_tids;
            run_n_tuples = total_postings / 5;
            if (run_n_tuples == 0 && total_postings > 0)
                run_n_tuples = 1;
        }

        memset(&run, 0, sizeof(run));
        run.level = 1;
        run.flags = PG_TRE_RUN_LIVE;
        run.root_upper = run_root;
        run.root_range = InvalidBlockNumber;   /* runs have no range tier yet */
        run.n_trigrams = (uint64) k;
        run.n_tuples = run_n_tuples;
        run.min_trigram_hash = lo_hash;
        run.max_trigram_hash = hi_hash;
        (void) pg_tre_run_catalog_append(index, &run);

        /* Truncate the consumed pending prefix; keep the base root
         * (no-op root swap), so finalize_merge only does the truncate. */
        finalize_merge(index, meta.root_upper, meta.root_range,
                       meta.n_trigrams, meta.n_tuples_indexed,
                       wm_tail, wm_tail_n, &consumed);
        n_merged = consumed;

        MemoryContextSwitchTo(old);
        MemoryContextDelete(merge_cxt);
        return n_merged;
    }

    snapshot_existing_upper(index, &rs, merge_cxt);

    /* Rebuild the upper tree from the merged stream. */
    new_root = pg_tre_upper_bulkload(index, rebuild_iter, &rs);

    /*
     * Atomically swap in the new root_upper AND truncate the consumed
     * prefix of the pending list in one WAL record.  n_tuples_indexed
     * is bumped by an approximation of the distinct TIDs we ingested
     * (a TID recurs across ~5 trigrams); the exact consumed count is
     * returned by finalize_merge so the estimate tracks what was
     * actually merged rather than the whole (possibly grown) list.
     */
    {
        uint64 consumed = 0;
        uint64 new_n_tuples;
        uint64 tid_est;

        finalize_merge(index, new_root, meta.root_range,
                       (uint64) k, meta.n_tuples_indexed,
                       wm_tail, wm_tail_n, &consumed);

        n_merged = consumed;
        tid_est = consumed / 5;   /* avg ~5 trigrams per row */
        if (tid_est > 0)
        {
            new_n_tuples = meta.n_tuples_indexed + tid_est;
            /*
             * finalize_merge already wrote n_tuples_indexed =
             * meta.n_tuples_indexed; patch in the bumped estimate with
             * a plain meta update.  This is a stats-only field, so a
             * crash that loses it merely reverts to a slightly stale
             * planner estimate -- never a correctness issue, and never
             * affects pending-list atomicity.
             */
            pg_tre_meta_set_roots(index, new_root, meta.root_range,
                                  (uint64) k, new_n_tuples);
        }
    }

    /* No sparsemap handles to free (tids arrays are palloc'd in merge_cxt
     * and released by MemoryContextDelete below). */

    MemoryContextSwitchTo(old);
    MemoryContextDelete(merge_cxt);

    return n_merged;
}

/*
 * Snapshot one run's upper-tree leaves and fold every (trigram, TID)
 * it holds into the MergeCtx (Phase B1.4 collapse).  Reuses the
 * upper-tree walker (snapshot_upper_subtree) to enumerate the run's
 * trigrams, then materializes each posting and enumerates its TIDs
 * into the per-trigram accumulator.
 */
static void
collapse_fold_run(Relation index, BlockNumber root_upper,
                  MergeCtx *mc, MemoryContext mcxt)
{
    RebuildState rs;
    int i;

    if (root_upper == InvalidBlockNumber)
        return;

    memset(&rs, 0, sizeof(rs));
    snapshot_upper_subtree(index, &rs, mcxt, root_upper);

    for (i = 0; i < rs.n_existing; i++)
    {
        PgTreUpperLeafEntry *e = &rs.existing[i];
        sm_t   *sm;
        uint64  idx;
        MergeEntry *me;

        if ((e->inline_bytes & PG_TRE_COALESCED_FLAG) != 0)
        {
            /*
             * Coalesced entry (v8): resolve the slot on its shared
             * coalesced page to the inline sparsemap bytes, then
             * materialize from those.  posting_root here is the
             * coalesced page block, not a posting-tree root.
             */
            uint16        slot = (uint16) (e->inline_bytes
                                           & ~PG_TRE_COALESCED_FLAG);
            const uint8  *blob;
            Size          blob_len = 0;

            blob = pg_tre_coalesced_resolve_slot(index, e->posting_root,
                                                 slot, e->trigram_hash,
                                                 &blob_len);
            if (blob == NULL || blob_len == 0)
                continue;
            sm = pg_tre_posting_materialize(index, InvalidBlockNumber,
                                            blob, blob_len);
        }
        else
        {
            sm = pg_tre_posting_materialize(index, e->posting_root,
                                            rs.existing_inline[i],
                                            e->inline_bytes);
        }
        if (sm == NULL)
            continue;

        me = merge_find(mc, e->trigram_hash, true);
        idx = SM_IDX_MAX;
        while ((idx = sm_next_member(sm, idx)) != SM_IDX_MAX)
        {
            if (me->n_tids >= me->tids_alloced)
            {
                MemoryContext o = MemoryContextSwitchTo(mc->mcxt);
                me->tids_alloced *= 2;
                me->tids = repalloc(me->tids,
                                    me->tids_alloced * sizeof(uint64));
                MemoryContextSwitchTo(o);
            }
            me->tids[me->n_tids++] = idx;
        }
        free(sm);
    }
}

/*
 * Collapse ALL live runs (base + catalog) into a single fresh base
 * run (Phase B1.4 adaptive collapse).  Bounds run growth: a
 * quiescent index converges to the pre-v7 single-structure layout,
 * so scan cost returns to baseline.  Driven from VACUUM.
 *
 * The merge is in TID space: fold every run's (trigram, TID) into a
 * MergeCtx (de-duped per trigram by the sparsemap), materialize the
 * unioned postings, bulkload a fresh upper tree, then atomically swap
 * the meta root and reset the catalog to single-run
 * (pg_tre_run_catalog_collapse_reset).  Returns the number of runs
 * collapsed (0 if there was nothing to do).
 */
uint32
pg_tre_collapse_runs(Relation index)
{
    PgTreMetaPageData meta;
    MemoryContext     cxt, old;
    MergeCtx          mc;
    RebuildState      rs;
    PgTreRunIter     *it;
    PgTreRun          run;
    uint32            n_runs;
    int               i, k;
    BlockNumber       new_root;
    BlockNumber      *dropped_pages = NULL;
    int               n_dropped = 0;
    int               dropped_cap = 0;

    n_runs = pg_tre_run_count(index);
    if (n_runs <= 1)
        return 0;            /* already a single run -- nothing to do */

    pg_tre_meta_read(index, &meta);

    cxt = AllocSetContextCreate(CurrentMemoryContext,
                                "pg_tre collapse", ALLOCSET_DEFAULT_SIZES);
    old = MemoryContextSwitchTo(cxt);

    memset(&mc, 0, sizeof(mc));
    mc.bucket_cap = 256;
    mc.bucket_tab = MemoryContextAllocZero(cxt,
                       mc.bucket_cap * sizeof(MergeEntry *));
    mc.mcxt       = cxt;
    mc.pending_only = true;   /* postings come from the runs themselves */

    /* Fold every live run's contents into the merge accumulator, and
     * collect each run's pages (in the OUTER context) for deferred free:
     * after the reset below, ALL old runs -- base included -- become
     * unreachable, replaced by the freshly bulkloaded new_root. */
    it = pg_tre_run_catalog_open(index);
    while (pg_tre_run_catalog_next(it, &run))
    {
        collapse_fold_run(index, run.root_upper, &mc, cxt);
        {
            int          rn = 0;
            BlockNumber *rp;
            MemoryContext o2 = MemoryContextSwitchTo(old);
            rp = pg_tre_run_collect_pages(index, run.root_upper, &rn);
            if (rn > 0)
            {
                if (n_dropped + rn > dropped_cap)
                {
                    int nc = (dropped_cap == 0)
                                ? (n_dropped + rn) : dropped_cap * 2;
                    if (nc < n_dropped + rn)
                        nc = n_dropped + rn;
                    dropped_pages = (dropped_pages == NULL)
                        ? palloc(nc * sizeof(BlockNumber))
                        : repalloc(dropped_pages, nc * sizeof(BlockNumber));
                    dropped_cap = nc;
                }
                memcpy(dropped_pages + n_dropped, rp, rn * sizeof(BlockNumber));
                n_dropped += rn;
                pfree(rp);
            }
            MemoryContextSwitchTo(o2);
        }
    }
    pg_tre_run_catalog_close(it);

    /* Materialize the per-trigram unioned postings (no existing-union;
     * the runs ARE the source). */
    materialize_merged_postings(index, &mc);

    rs.sorted = MemoryContextAlloc(cxt, mc.n_entries * sizeof(MergeEntry *));
    k = 0;
    for (i = 0; i < mc.bucket_cap; i++)
        if (mc.bucket_tab[i] != NULL)
            rs.sorted[k++] = mc.bucket_tab[i];
    Assert(k == mc.n_entries);
    qsort(rs.sorted, k, sizeof(MergeEntry *), cmp_merge_hash);
    rs.n_sorted  = k;
    rs.i_sorted  = 0;
    rs.existing = NULL;
    rs.existing_inline = NULL;
    rs.n_existing = 0;
    rs.i_existing = 0;
    rs.existing_cap = 0;

    new_root = pg_tre_upper_bulkload(index, rebuild_iter, &rs);

    /* Atomically install the collapsed tree + reset the catalog. */
    pg_tre_run_catalog_collapse_reset(index, new_root, meta.root_range,
                                      (uint64) k, meta.n_tuples_indexed);

    MemoryContextSwitchTo(old);
    MemoryContextDelete(cxt);

    /* Defer-free the old runs' pages now that they are unreachable. */
    if (n_dropped > 0)
    {
        pg_tre_free_log_append(index, dropped_pages, n_dropped);
        pfree(dropped_pages);
    }
    return n_runs;
}

/*
 * Hanoi incremental level-merge (Phase B1.4 refinement).
 *
 * Instead of folding ALL runs into the base at once (O(runs x index),
 * a multi-minute stall at hundreds of runs), merge only ONE
 * over-capacity Hanoi level at a time: level L holds at most 2^(L-1)
 * runs; when it exceeds that, merge exactly those level-L runs into a
 * single new run promoted to level L+1, leaving the base run and all
 * other levels untouched.  Per-merge work is bounded by one level's
 * runs, not the whole catalog.
 *
 * Cascades: promoting into level L+1 may push it over capacity, so we
 * loop until no level exceeds its cap (or we hit max_levels, beyond
 * which the deepest level just accumulates -- the caller can still
 * fall back to a full collapse if it wants single-run convergence).
 *
 * Returns the number of merge passes performed (0 if nothing was over
 * capacity).  Driven from VACUUM.  Caller holds a lock excluding
 * concurrent catalog writers (the index's ShareUpdateExclusive).
 */
#define PG_TRE_MAX_CATALOG_RUNS ((int) RUN_CATALOG_CAP)

/*
 * True if `root` is referenced by any run in runs[0..n_runs) at a
 * different level than `dropped_level`, or equals the base run's root
 * `base_root`.  A run whose root is shared this way must NOT have its
 * pages defer-freed when it is dropped: the surviving run (or the base)
 * still reaches those exact pages.  This guards the case where runs
 * alias the base structure -- e.g. tre_debug_append_run registers runs
 * sharing meta.root_upper -- so a Hanoi merge of those runs never frees
 * the live base tree.  (Production flush-to-run builds a distinct fresh
 * upper tree per run, so its roots never alias and every dropped run is
 * freed.)
 */
static bool
root_is_shared(BlockNumber root, BlockNumber base_root,
               const PgTreRun *runs, int n_runs, uint32 dropped_level)
{
    int i;

    if (!BlockNumberIsValid(root))
        return true;            /* nothing to free anyway */
    if (root == base_root)
        return true;
    for (i = 0; i < n_runs; i++)
        if (runs[i].root_upper == root &&
            (uint32) runs[i].level != dropped_level)
            return true;
    return false;
}

uint32
pg_tre_hanoi_merge(Relation index)
{
    PgTreMetaPageData meta;
    uint32  max_levels;
    uint32  passes = 0;
    int     guard;

    pg_tre_meta_read(index, &meta);
    max_levels = (meta.max_levels > 0) ? meta.max_levels : 7;

    /* Bounded loop: at most one promotion per level, a few cascades. */
    for (guard = 0; guard < 64; guard++)
    {
        PgTreRun      runs[PG_TRE_MAX_CATALOG_RUNS];
        int           n_runs = 0;
        PgTreRunIter *it;
        PgTreRun      run;
        uint32        target_level = 0;
        int           per_level[64];
        int           lv;

        memset(per_level, 0, sizeof(per_level));

        /* Collect the catalog runs (skip the implicit base, run_id 0). */
        it = pg_tre_run_catalog_open(index);
        while (pg_tre_run_catalog_next(it, &run))
        {
            if (run.run_id == 0)
                continue;           /* base run is not catalog-merged */
            if (n_runs >= PG_TRE_MAX_CATALOG_RUNS)
                break;
            runs[n_runs++] = run;
            lv = (run.level >= 1 && run.level < 64) ? (int) run.level : 1;
            per_level[lv]++;
        }
        pg_tre_run_catalog_close(it);

        /* Find the shallowest level that exceeds its Hanoi capacity
         * 2^(L-1).  Do not promote past max_levels. */
        for (lv = 1; lv < (int) max_levels && lv < 64; lv++)
        {
            int cap = 1 << (lv - 1);
            if (per_level[lv] > cap)
            {
                target_level = (uint32) lv;
                break;
            }
        }
        if (target_level == 0)
            break;              /* no level over capacity -- done */

        /* Merge all runs at target_level into one promoted run. */
        {
            MemoryContext cxt, old;
            MergeCtx      mc;
            RebuildState  rs;
            PgTreRun      survivors[PG_TRE_MAX_CATALOG_RUNS];
            PgTreRun      promoted;
            int           n_surv = 0, i, k;
            uint64        lo = UINT64_MAX, hi = 0, ntup = 0;
            BlockNumber   new_root;
            uint64        new_id;
            BlockNumber  *dropped_pages = NULL;
            int           n_dropped = 0;
            int           dropped_cap = 0;

            cxt = AllocSetContextCreate(CurrentMemoryContext,
                       "pg_tre hanoi merge", ALLOCSET_DEFAULT_SIZES);
            old = MemoryContextSwitchTo(cxt);

            memset(&mc, 0, sizeof(mc));
            mc.bucket_cap = 256;
            mc.bucket_tab = MemoryContextAllocZero(cxt,
                               mc.bucket_cap * sizeof(MergeEntry *));
            mc.mcxt = cxt;
            mc.pending_only = true;

            for (i = 0; i < n_runs; i++)
            {
                if ((int) runs[i].level == (int) target_level)
                {
                    collapse_fold_run(index, runs[i].root_upper, &mc, cxt);
                    if (runs[i].min_trigram_hash < lo) lo = runs[i].min_trigram_hash;
                    if (runs[i].max_trigram_hash > hi) hi = runs[i].max_trigram_hash;
                    ntup += runs[i].n_tuples;

                    /*
                     * Collect this dropped run's pages (in the OUTER
                     * context so they survive the cxt delete below) for
                     * deferred free.  Skip runs whose root is shared by a
                     * surviving run or the base (root_is_shared) -- those
                     * pages are still live.  Done while the run is still
                     * in the catalog and fully traversable; the actual
                     * free is XID-gated in a later VACUUM via the free
                     * log.
                     */
                    if (!root_is_shared(runs[i].root_upper, meta.root_upper,
                                        runs, n_runs, target_level))
                    {
                        int          rn = 0;
                        BlockNumber *rp;
                        MemoryContext o2 = MemoryContextSwitchTo(old);
                        rp = pg_tre_run_collect_pages(index,
                                                      runs[i].root_upper, &rn);
                        if (rn > 0)
                        {
                            if (n_dropped + rn > dropped_cap)
                            {
                                int nc = (dropped_cap == 0)
                                            ? (n_dropped + rn) : dropped_cap * 2;
                                if (nc < n_dropped + rn)
                                    nc = n_dropped + rn;
                                dropped_pages = (dropped_pages == NULL)
                                    ? palloc(nc * sizeof(BlockNumber))
                                    : repalloc(dropped_pages,
                                               nc * sizeof(BlockNumber));
                                dropped_cap = nc;
                            }
                            memcpy(dropped_pages + n_dropped, rp,
                                   rn * sizeof(BlockNumber));
                            n_dropped += rn;
                            pfree(rp);
                        }
                        MemoryContextSwitchTo(o2);
                    }
                }
                else
                {
                    survivors[n_surv++] = runs[i];
                }
            }

            materialize_merged_postings(index, &mc);

            rs.sorted = MemoryContextAlloc(cxt,
                          mc.n_entries * sizeof(MergeEntry *));
            k = 0;
            for (i = 0; i < mc.bucket_cap; i++)
                if (mc.bucket_tab[i] != NULL)
                    rs.sorted[k++] = mc.bucket_tab[i];
            Assert(k == mc.n_entries);
            qsort(rs.sorted, k, sizeof(MergeEntry *), cmp_merge_hash);
            rs.n_sorted = k; rs.i_sorted = 0;
            rs.existing = NULL; rs.existing_inline = NULL;
            rs.n_existing = 0; rs.i_existing = 0; rs.existing_cap = 0;

            new_root = pg_tre_upper_bulkload(index, rebuild_iter, &rs);

            /* Allocate a fresh run_id for the promoted run by appending
             * then immediately folding it into the rewrite -- but to
             * keep next_run_id monotonic without a separate append, we
             * reserve an id via a catalog append of the promoted run,
             * then rewrite the full survivors+promoted list. */
            memset(&promoted, 0, sizeof(promoted));
            promoted.level = target_level + 1;
            promoted.flags = PG_TRE_RUN_LIVE;
            promoted.root_upper = new_root;
            promoted.root_range = InvalidBlockNumber;
            promoted.n_tuples = ntup;
            promoted.n_trigrams = (uint64) k;
            promoted.min_trigram_hash = (k > 0) ? lo : 0;
            promoted.max_trigram_hash = (k > 0) ? hi : UINT64_MAX;
            new_id = pg_tre_run_catalog_append(index, &promoted);
            promoted.run_id = new_id;

            /* Rewrite the catalog as survivors + promoted (the append
             * above advanced next_run_id and added promoted to the
             * catalog; rewrite installs the deduplicated final list and
             * drops the merged level-L runs). */
            survivors[n_surv++] = promoted;
            pg_tre_run_catalog_rewrite(index, survivors, n_surv);

            MemoryContextSwitchTo(old);
            MemoryContextDelete(cxt);

            /*
             * The level-L runs are now out of the catalog.  Record their
             * pages for XID-gated deferred free (dropped_pages lives in
             * `old`, which survives the cxt delete above).  Append after
             * the rewrite: a crash in between only leaks the pages (a
             * later full reclaim / REINDEX recovers them), never frees a
             * page a scan still follows.
             */
            if (n_dropped > 0)
            {
                pg_tre_free_log_append(index, dropped_pages, n_dropped);
                pfree(dropped_pages);
            }
        }
        passes++;
    }

    return passes;
}

/* --------------------------------------------------------------------
 * WAL redo helper (called from src/wal/xlog.c)
 * -------------------------------------------------------------------- */

bool
pg_tre_pending_redo_apply_delta(Page page,
                                uint16 prev_n_entries,
                                uint16 take,
                                const PgTrePendingEntry *src_entries)
{
    PgTrePendingHeader *hdr     = pending_header(page);
    PgTrePendingEntry  *entries = pending_entries(page);

    if (hdr->n_entries != prev_n_entries)
        return false;

    memcpy(&entries[prev_n_entries], src_entries,
           (size_t) take * sizeof(PgTrePendingEntry));

    hdr->n_entries  += take;
    hdr->used_bytes  = hdr->n_entries * sizeof(PgTrePendingEntry);
    ((PageHeader) page)->pd_lower =
        (char *) &entries[hdr->n_entries] - (char *) page;

    return true;
}
