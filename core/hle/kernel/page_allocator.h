/**
 * Guest physical page allocator. See docs/DESIGN.md §4 ("guest RAM") and
 * §12 ("Process bootstrap", step 3).
 *
 * vmm_map() takes a guest PHYSICAL address (§5); somebody has to hand
 * those out. This is Horizon's KMemoryManager in its smallest useful
 * form: a bump allocator over the layout's guest RAM region (§4), in
 * VMM_PAGE_SIZE units, plus a small freelist for release. Physical
 * addresses start at 0 (guest_pa n is linear-memory offset
 * guest_ram_base + n); the allocator never touches the bytes, only
 * accounts for them.
 *
 * Release: the bootstrap's own allocations (modules, stack, TLS pages)
 * still never free individually - a process image lives until the
 * process dies and is reclaimed as a whole by page_allocator_reset().
 * The memory HLE (hle/kernel/svc_memory.h) is
 * the first real caller of page_allocator_free(): svcSetHeapSize
 * shrinking the heap's tail, and process_teardown() reclaiming whatever
 * heap remained committed. The freelist exists for THIS caller: an
 * out-of-order free (not the most recently bumped run) is expected, but
 * coalescing is deliberately minimal (see page_allocator_free) rather
 * than a general-purpose allocator's - Phase 1 has exactly one shrinking
 * consumer.
 *
 * `used_bytes` is what GetInfo's UsedMemorySize will report (§12) - it is
 * the bump high-water mark, not "bump minus freed"; freed-but-not-yet-
 * reused pages are still "used" from guest RAM's perspective until this
 * allocator hands them to someone else or the process dies.
 */
#ifndef SWITCH_HLE_KERNEL_PAGE_ALLOCATOR_H
#define SWITCH_HLE_KERNEL_PAGE_ALLOCATOR_H

#include <stdint.h>

#include "common/result.h"

/* A run in the freelist: `page_count` pages starting at guest physical
 * address `pa`. */
typedef struct Page_Run {
  uint64_t pa;
  uint64_t page_count;
} Page_Run;

/* Freelist capacity. Fixed and small: Phase 1's only source of frees is
 * svcSetHeapSize shrinking (at most one run per call) and process
 * teardown (one run for the whole committed heap) - this is not a
 * general-purpose allocator. page_allocator_free() returns
 * RESULT_OUT_OF_MEMORY, unchanged, if a caller ever exceeds this; revisit
 * the cap if that fires for real. */
#define PAGE_ALLOCATOR_MAX_FREE_RUNS 16u

typedef struct Page_Allocator {
  uint64_t base_pa;    /* first allocatable guest physical address */
  uint64_t size_bytes; /* extent; page multiple */
  uint64_t used_bytes; /* bump high-water mark, page multiple: base_pa +
                        * used_bytes is the bump cursor */
  Page_Run free_runs[PAGE_ALLOCATOR_MAX_FREE_RUNS];
  uint32_t free_run_count;
} Page_Allocator;

/* Initializes an allocator over [base_pa, base_pa + size_bytes) of guest
 * physical space. RESULT_INVALID_ARGUMENT if `out` is NULL, either value
 * is not page-aligned, size_bytes == 0, or the range exceeds the live
 * layout's guest_ram_size (requires layout_create()). */
Error page_allocator_init(Page_Allocator *out, uint64_t base_pa, uint64_t size_bytes);

/* Convenience: the allocator that spans all of guest RAM. */
Error page_allocator_init_guest_ram(Page_Allocator *out);

/* Allocates `page_count` contiguous pages; `*out_pa` receives the first
 * page's guest physical address. Checks the freelist first (first-fit): a
 * run at least `page_count` pages long is taken from it, splitting the
 * remainder back into the freelist if it is larger than requested; only
 * when no freelist run fits does the bump cursor advance.
 * RESULT_INVALID_ARGUMENT for NULL args or page_count == 0;
 * RESULT_OUT_OF_MEMORY when no freelist run fits and fewer pages remain
 * ahead of the cursor than requested (the allocator is unchanged).
 * Contents are NOT zeroed: the caller that maps the pages decides
 * (bootstrap zeroes .bss, stack and TLS explicitly; a reused freed run may
 * still hold a previous owner's bytes). */
Error page_allocator_allocate(Page_Allocator *allocator, uint64_t page_count,
                              uint64_t *out_pa);

/* Returns `page_count` pages starting at `pa` to the allocator. The range
 * must describe pages this allocator previously handed out via
 * page_allocator_allocate (page-aligned, wholly inside [base_pa, base_pa +
 * size_bytes)); RESULT_INVALID_ARGUMENT otherwise. The allocator does not
 * track individual outstanding allocations, so it cannot detect a
 * double-free or a range that was never allocated - callers are trusted
 * the same way vmm_unmap trusts its caller to already know what is mapped.
 *
 * A run ending exactly at the bump cursor retracts the cursor directly
 * (used_bytes shrinks) rather than entering the freelist, so a
 * shrink-then-regrow of the same tail never costs freelist capacity.
 * Every other run is pushed onto the freelist, coalescing with an
 * existing entry only when the two runs are directly adjacent (no
 * general neighbor search beyond that) - RESULT_OUT_OF_MEMORY if the
 * freelist is full and the run cannot be placed or merged; the allocator
 * is unchanged. */
Error page_allocator_free(Page_Allocator *allocator, uint64_t pa, uint64_t page_count);

/* Bytes still allocatable. */
uint64_t page_allocator_free_bytes(const Page_Allocator *allocator);

/* Forgets every allocation and clears the freelist. Only valid when no
 * mapping still refers to the pages (a failed bootstrap has already
 * unmapped its own). */
void page_allocator_reset(Page_Allocator *allocator);

#endif /* SWITCH_HLE_KERNEL_PAGE_ALLOCATOR_H */
