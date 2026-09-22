/**
 * Guest physical page allocator. See page_allocator.h.
 */
#include "hle/kernel/page_allocator.h"

#include "common/layout.h"
#include "common/vmm.h"

#include <string.h>

Error page_allocator_init(Page_Allocator *out, uint64_t base_pa, uint64_t size_bytes) {
  if (!out) {
    return ERR(RESULT_INVALID_ARGUMENT, "page_allocator_init: out is NULL");
  }
  const Memory_Layout *layout = layout_get();
  if (!layout) {
    return ERR(RESULT_INVALID_ARGUMENT, "page_allocator_init: no live layout");
  }
  if ((base_pa & VMM_PAGE_OFFSET_MASK) != 0 || (size_bytes & VMM_PAGE_OFFSET_MASK) != 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "page_allocator_init: range not page-aligned");
  }
  if (size_bytes == 0 || base_pa > layout->guest_ram_size ||
      size_bytes > layout->guest_ram_size - base_pa) {
    return ERR(RESULT_INVALID_ARGUMENT, "page_allocator_init: range outside guest RAM");
  }
  out->base_pa = base_pa;
  out->size_bytes = size_bytes;
  out->used_bytes = 0;
  out->free_run_count = 0;
  return OK;
}

Error page_allocator_init_guest_ram(Page_Allocator *out) {
  const Memory_Layout *layout = layout_get();
  if (!layout) {
    return ERR(RESULT_INVALID_ARGUMENT, "page_allocator_init_guest_ram: no live layout");
  }
  return page_allocator_init(out, 0, layout->guest_ram_size);
}

Error page_allocator_allocate(Page_Allocator *allocator, uint64_t page_count,
                              uint64_t *out_pa) {
  if (!allocator || !out_pa) {
    return ERR(RESULT_INVALID_ARGUMENT, "page_allocator_allocate: NULL argument");
  }
  if (page_count == 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "page_allocator_allocate: zero pages");
  }

  /* First-fit over the freelist before touching the bump cursor (see
   * page_allocator.h: this is the release path's whole reason to exist -
   * a shrunk heap's pages must be reusable by a later regrow). A run
   * larger than requested is split; the remainder stays in the freelist. */
  for (uint32_t i = 0; i < allocator->free_run_count; i++) {
    Page_Run *run = &allocator->free_runs[i];
    if (run->page_count >= page_count) {
      *out_pa = run->pa;
      if (run->page_count == page_count) {
        allocator->free_runs[i] = allocator->free_runs[--allocator->free_run_count];
      } else {
        run->pa += page_count << VMM_PAGE_BITS;
        run->page_count -= page_count;
      }
      return OK;
    }
  }

  /* page_count * PAGE_SIZE can overflow for a hostile count; compare in
   * pages instead. */
  const uint64_t free_pages = (allocator->size_bytes - allocator->used_bytes) >> VMM_PAGE_BITS;
  if (page_count > free_pages) {
    return ERR(RESULT_OUT_OF_MEMORY, "page_allocator_allocate: guest RAM exhausted");
  }
  *out_pa = allocator->base_pa + allocator->used_bytes;
  allocator->used_bytes += page_count << VMM_PAGE_BITS;
  return OK;
}

Error page_allocator_free(Page_Allocator *allocator, uint64_t pa, uint64_t page_count) {
  if (!allocator) {
    return ERR(RESULT_INVALID_ARGUMENT, "page_allocator_free: NULL allocator");
  }
  if (page_count == 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "page_allocator_free: zero pages");
  }
  if ((pa & VMM_PAGE_OFFSET_MASK) != 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "page_allocator_free: pa not page-aligned");
  }
  if (pa < allocator->base_pa) {
    return ERR(RESULT_INVALID_ARGUMENT, "page_allocator_free: pa before base_pa");
  }
  /* Compare in pages, not bytes, so a hostile page_count cannot overflow
   * the range check (same reasoning as page_allocator_allocate). */
  const uint64_t offset_pages = (pa - allocator->base_pa) >> VMM_PAGE_BITS;
  const uint64_t used_pages = allocator->used_bytes >> VMM_PAGE_BITS;
  if (offset_pages >= used_pages || page_count > used_pages - offset_pages) {
    return ERR(RESULT_INVALID_ARGUMENT,
               "page_allocator_free: range was never handed out by this allocator");
  }

  const uint64_t size_bytes = page_count << VMM_PAGE_BITS;
  const uint64_t end = pa + size_bytes;
  const uint64_t cursor = allocator->base_pa + allocator->used_bytes;

  if (end == cursor) {
    /* Retracts the cursor directly rather than entering the freelist, so
     * a shrink-then-regrow of the same tail never costs freelist
     * capacity. Then one absorption pass: if a freelist run now ends
     * exactly at the retracted cursor, fold it in too. Deliberately not
     * a loop to a fixed point (page_allocator.h: minimal coalescing, not
     * a general-purpose allocator's) - a chain of several such runs
     * stays as several freelist entries. */
    allocator->used_bytes -= size_bytes;
    const uint64_t new_cursor = allocator->base_pa + allocator->used_bytes;
    for (uint32_t i = 0; i < allocator->free_run_count; i++) {
      Page_Run *run = &allocator->free_runs[i];
      if (run->pa + (run->page_count << VMM_PAGE_BITS) == new_cursor) {
        allocator->used_bytes -= run->page_count << VMM_PAGE_BITS;
        allocator->free_runs[i] = allocator->free_runs[--allocator->free_run_count];
        break;
      }
    }
    return OK;
  }

  /* Not adjacent to the cursor: coalesce with at most one existing
   * freelist entry directly before or after this run, else push a new
   * entry. No general neighbor search beyond that one check each way -
   * same minimal-coalescing policy as above. */
  for (uint32_t i = 0; i < allocator->free_run_count; i++) {
    Page_Run *run = &allocator->free_runs[i];
    if (run->pa == end) {
      run->pa = pa;
      run->page_count += page_count;
      return OK;
    }
    if (run->pa + (run->page_count << VMM_PAGE_BITS) == pa) {
      run->page_count += page_count;
      return OK;
    }
  }
  if (allocator->free_run_count >= PAGE_ALLOCATOR_MAX_FREE_RUNS) {
    return ERR(RESULT_OUT_OF_MEMORY, "page_allocator_free: freelist full");
  }
  allocator->free_runs[allocator->free_run_count++] = (Page_Run){.pa = pa, .page_count = page_count};
  return OK;
}

uint64_t page_allocator_free_bytes(const Page_Allocator *allocator) {
  if (!allocator) return 0;
  uint64_t free_bytes = allocator->size_bytes - allocator->used_bytes;
  for (uint32_t i = 0; i < allocator->free_run_count; i++) {
    free_bytes += allocator->free_runs[i].page_count << VMM_PAGE_BITS;
  }
  return free_bytes;
}

void page_allocator_reset(Page_Allocator *allocator) {
  if (!allocator) return;
  allocator->used_bytes = 0;
  allocator->free_run_count = 0;
}
