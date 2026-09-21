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

uint64_t page_allocator_free_bytes(const Page_Allocator *allocator) {
  if (!allocator) return 0;
  return allocator->size_bytes - allocator->used_bytes;
}

void page_allocator_reset(Page_Allocator *allocator) {
  if (!allocator) return;
  allocator->used_bytes = 0;
}
