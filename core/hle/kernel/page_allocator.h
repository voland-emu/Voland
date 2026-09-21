/**
 * Guest physical page allocator. See docs/DESIGN.md §4 ("guest RAM") and
 * §12 ("Process bootstrap", step 3).
 *
 * vmm_map() takes a guest PHYSICAL address (§5); somebody has to hand
 * those out. This is Horizon's KMemoryManager in its smallest useful
 * form: a bump allocator over the layout's guest RAM region (§4), in
 * VMM_PAGE_SIZE units. Physical addresses start at 0 (guest_pa n is
 * linear-memory offset guest_ram_base + n); the allocator never touches
 * the bytes, only accounts for them.
 *
 * Phase 1 scope, stated: allocation only. Nothing in the bootstrap frees
 * (a process image lives until the process dies), and the memory HLE
 * that lands next (SetHeapSize shrink, UnmapMemory) is the first caller
 * that needs release. When it arrives, this grows a page-run freelist
 * behind the same interface; callers are not expected to change.
 *
 * `used_bytes` is what GetInfo's UsedMemorySize will report (§12).
 */
#ifndef SWITCH_HLE_KERNEL_PAGE_ALLOCATOR_H
#define SWITCH_HLE_KERNEL_PAGE_ALLOCATOR_H

#include <stdint.h>

#include "common/result.h"

typedef struct Page_Allocator {
  uint64_t base_pa;    /* first allocatable guest physical address */
  uint64_t size_bytes; /* extent; page multiple */
  uint64_t used_bytes; /* bump pointer, page multiple */
} Page_Allocator;

/* Initializes an allocator over [base_pa, base_pa + size_bytes) of guest
 * physical space. RESULT_INVALID_ARGUMENT if `out` is NULL, either value
 * is not page-aligned, size_bytes == 0, or the range exceeds the live
 * layout's guest_ram_size (requires layout_create()). */
Error page_allocator_init(Page_Allocator *out, uint64_t base_pa, uint64_t size_bytes);

/* Convenience: the allocator that spans all of guest RAM. */
Error page_allocator_init_guest_ram(Page_Allocator *out);

/* Allocates `page_count` contiguous pages; `*out_pa` receives the first
 * page's guest physical address. RESULT_INVALID_ARGUMENT for NULL args
 * or page_count == 0; RESULT_OUT_OF_MEMORY when fewer pages remain (the
 * allocator is unchanged). Contents are NOT zeroed: the caller that maps
 * the pages decides (bootstrap zeroes .bss, stack and TLS explicitly). */
Error page_allocator_allocate(Page_Allocator *allocator, uint64_t page_count,
                              uint64_t *out_pa);

/* Bytes still allocatable. */
uint64_t page_allocator_free_bytes(const Page_Allocator *allocator);

/* Forgets every allocation. Only valid when no mapping still refers to
 * the pages (a failed bootstrap has already unmapped its own). */
void page_allocator_reset(Page_Allocator *allocator);

#endif /* SWITCH_HLE_KERNEL_PAGE_ALLOCATOR_H */
