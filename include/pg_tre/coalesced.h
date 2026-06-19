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
 * Maximum serialized sparsemap size that is eligible for a coalesced
 * page.  Postings at or below PG_TRE_INLINE_POSTING_MAX go inline in the
 * upper leaf; postings above the cap get a dedicated posting tree.  The
 * medium bucket in between is coalesced.
 *
 * The cap is the largest blob for which AT LEAST TWO slots still fit on
 * one page:
 *
 *     2 * (sizeof(slot) + MAXALIGN(cap)) <= pg_tre_coalesced_budget()
 *
 * A page that fits only one slot is no denser than today's dedicated
 * leaf (and worse, because a coalesced slot drops its per-tuple payload
 * -- the tier-3 pre-filter -- which a dedicated leaf keeps).  So the cap
 * is pinned at the >=2-slots boundary: below it coalescing strictly wins
 * on page count; above it a dedicated leaf is the right call.
 *
 * History: 2.0 shipped a fixed 3072, which only ever packed ~2 slots and
 * left the entire (3072, single-leaf-budget] band -- the bulk of
 * medium/high-cardinality trigrams on real corpora -- in dedicated
 * one-page-each leaves (the 60.9 KB/row density blocker).  Widening to
 * the >=2-slots boundary lets the existing coalesced writer absorb that
 * whole band: postings in (2048, ~4056] now share pages 2-3 to a page
 * instead of one each.  Additive: still gated by pg_tre.coalesce_enable,
 * still format v8, no on-disk struct change.  See
 * doc/specs/posting-page-coalescing.md and
 * .agent/notes/blocker1-density-brief.md.
 *
 * Defined as an inline (pg_tre_coalesce_max) just below
 * pg_tre_coalesced_budget(), which it depends on.
 */

/*
 * Sentinel for pg_tre_coalesced_resolve_slot's trigram_hash argument:
 * skip the hash-equality check (the slot's own stored hash is
 * authoritative).  Use only when resolving a marker that came from the
 * index's own upper tree (build / merge / range paths) where the hash
 * is not at hand.  The within-page bounds check still applies.
 */
#define PG_TRE_COALESCED_HASH_ANY ((uint64) 0xFFFFFFFFFFFFFFFFULL)

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
 * Largest serialized sparsemap eligible for coalescing: the biggest
 * sm_len for which AT LEAST TWO slots fit on one coalesced page.  Each
 * coalesced slot costs sizeof(PgTreCoalescedSlot) + MAXALIGN(sm_len)
 * (the coalesced path drops per-tuple payload, so only the sparsemap
 * counts).  Requiring two-per-page guarantees coalescing strictly beats
 * a dedicated single-page leaf; above this size a dedicated leaf is the
 * right call (it keeps the tier-3 payload and uses the same one page).
 * See the long-form rationale and history above pg_tre_coalesced_budget.
 */
static inline Size
pg_tre_coalesce_max(void)
{
    Size half = pg_tre_coalesced_budget() / 2;
    Size per_slot = half - sizeof(PgTreCoalescedSlot);
    /* Round down to a MAXALIGN boundary so MAXALIGN(cap) == cap <= per_slot,
     * i.e. 2 * (sizeof(slot) + MAXALIGN(cap)) <= budget holds exactly. */
    return per_slot & ~((Size) (MAXIMUM_ALIGNOF - 1));
}

/* Back-compat name used by the build-path eligibility gate. */
#define PG_TRE_COALESCE_MAX (pg_tre_coalesce_max())

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
