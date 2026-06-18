/*
 * include/pg_tre/coalesced.h - coalesced posting page API (format v8).
 *
 * A coalesced page (PG_TRE_PAGE_POSTING_COALESCED) packs the postings
 * of multiple trigrams onto one 8 KB page, addressed by a slot index
 * carried in the upper-tree leaf entry's inline_bytes field (high bit
 * PG_TRE_COALESCED_FLAG set).  See doc/specs/posting-page-coalescing.md.
 *
 * CONTRACT: the bump v7 -> v8 is purely additive.  A coalesced page is
 * only produced by a from-scratch build (Phase 2+, behind
 * pg_tre.coalesce_enable) or a future merge; v6/v7 indexes contain no
 * coalesced pages and read unchanged.
 */

#ifndef PG_TRE_COALESCED_H
#define PG_TRE_COALESCED_H

#include "postgres.h"
#include "storage/block.h"
#include "utils/rel.h"

#include "pg_tre/page.h"

/*
 * Maximum serialized sparsemap (+ payload) size that is eligible for a
 * coalesced page.  Postings at or below PG_TRE_INLINE_POSTING_MAX go
 * inline in the upper leaf; postings above PG_TRE_COALESCE_MAX get a
 * dedicated posting tree.  The medium bucket in between is coalesced.
 *
 * Cap chosen so a coalesced page packs at least ~2-3 slots (a page that
 * fits only one slot is no denser than today's dedicated leaf).
 */
#define PG_TRE_COALESCE_MAX 3072

/* Usable content bytes on a coalesced page (after header + opaque). */
static inline Size
pg_tre_coalesced_budget(void)
{
    return BLCKSZ
         - MAXALIGN(SizeOfPageHeaderData)
         - MAXALIGN(sizeof(PgTreCoalescedHeader))
         - MAXALIGN(sizeof(PageTreOpaqueData));
}

/*
 * Builder: accumulate (trigram_hash, sparsemap, payload) slots and emit
 * coalesced pages, first-fit by page budget.  Each emitted page is
 * WAL-logged as a full-page image (run-catalog writer pattern:
 * REGBUF_FORCE_IMAGE, all edits in one critical section).
 *
 * Use:
 *   w = pg_tre_coalesced_writer_begin(index);
 *   for each medium-bucket trigram:
 *       pg_tre_coalesced_add(w, hash, sm, sm_len, payload, payload_len,
 *                            &out_blk, &out_slot);
 *   pg_tre_coalesced_writer_finish(w);   // flush the last partial page
 *
 * out_blk / out_slot identify where the trigram landed; record them in
 * the upper-tree entry as posting_root = out_blk,
 * inline_bytes = PG_TRE_COALESCED_FLAG | out_slot.
 */
typedef struct PgTreCoalescedWriter PgTreCoalescedWriter;

extern PgTreCoalescedWriter *pg_tre_coalesced_writer_begin(Relation index);

extern void pg_tre_coalesced_add(PgTreCoalescedWriter *w,
                                 uint64 trigram_hash,
                                 const uint8 *sm_data, Size sm_len,
                                 const uint8 *payload_data, Size payload_len,
                                 BlockNumber *out_blk, uint16 *out_slot);

extern void pg_tre_coalesced_writer_finish(PgTreCoalescedWriter *w);

/*
 * Resolve a coalesced slot into a palloc'd copy of its sparsemap blob.
 * Returns the blob (caller owns; pfree or context reset) and writes its
 * length to *out_len.  trigram_hash is checked against the slot's
 * self-describing hash; a mismatch (torn/corrupt page) raises
 * ERROR data_corrupted.  Returns NULL for an INVALID (tombstoned) slot.
 */
extern uint8 *pg_tre_coalesced_resolve_slot(Relation index, BlockNumber blk,
                                            uint16 slot_idx,
                                            uint64 trigram_hash,
                                            Size *out_len);

#endif /* PG_TRE_COALESCED_H */
