/**
 * Guest TLS allocator. See tls.h for the contract and docs/DESIGN.md §12
 * for why every thread needs a block before IPC can exist.
 */
#include "hle/kernel/tls.h"

#include "common/log.h"

#include <string.h>

/* All eight blocks of a page allocated. */
#define TLS_PAGE_FULL_MASK ((uint8_t)((1u << TLS_BLOCKS_PER_PAGE) - 1u))
/* Sentinel for "no mapped page has a free block". */
#define TLS_NO_PAGE ((int32_t)-1)

/* Zeroes one block through a borrowed host pointer (the page is RW). */
static Error zero_block(VMM_Context *vmm, uint64_t gva) {
  void *host = NULL;
  vmm_borrow_scope_begin(vmm);
  Error err = vmm_guest_to_host(vmm, gva, TLS_BLOCK_BYTES, VMM_PERM_RW, &host);
  if (error_is_ok(err)) memset(host, 0, (size_t)TLS_BLOCK_BYTES);
  vmm_borrow_scope_end(vmm);
  return err;
}

/* Resolves `gva` to the mapped page holding it and the block index
 * within it. Returns false if the address is not the base of a block
 * on a mapped TLS page. */
static bool locate_block(const TLS_Allocator *allocator, uint64_t gva, uint32_t *out_page,
                         uint32_t *out_block) {
  if ((gva % TLS_BLOCK_BYTES) != 0) return false;
  for (uint32_t p = 0; p < allocator->page_count; p++) {
    const uint64_t page_gva = allocator->mapped[p].gva;
    if (gva >= page_gva && gva - page_gva < VMM_PAGE_SIZE) {
      *out_page = p;
      *out_block = (uint32_t)((gva - page_gva) / TLS_BLOCK_BYTES);
      return true;
    }
  }
  return false;
}

/* Block-selection policy: picks the mapped page to allocate from and
 * the block within it. Returns the page index, writing the block index
 * to `*out_block`, or TLS_NO_PAGE when every mapped page is full (the
 * caller then maps a fresh page and uses its block 0).
 *
 * The contract (tls.h) is "the lowest free block of the lowest page":
 * pages are in ascending address order in `mapped`, and bit b of
 * `used_mask` is block b. */
static int32_t pick_block(const TLS_Allocator *allocator, uint32_t *out_block) {
  for (uint32_t p = 0; p < allocator->page_count; p++) {
    const uint8_t used = allocator->mapped[p].used_mask;
    if (used == TLS_PAGE_FULL_MASK) continue;
    for (uint32_t b = 0; b < TLS_BLOCKS_PER_PAGE; b++) {
      if ((used & (1u << b)) == 0) {
        *out_block = b;
        return (int32_t)p;
      }
    }
  }
  return TLS_NO_PAGE;
}

/* Maps a fresh RW page at the next slot and records it. */
static Error map_new_page(TLS_Allocator *allocator) {
  if (allocator->page_count >= TLS_MAX_PAGES) {
    return ERR(RESULT_OUT_OF_MEMORY, "tls_allocate: TLS_MAX_PAGES reached");
  }
  const uint64_t gva = allocator->region.base + (uint64_t)allocator->page_count * VMM_PAGE_SIZE;
  if (!address_region_contains(&allocator->region, gva, VMM_PAGE_SIZE)) {
    return ERR(RESULT_OUT_OF_MEMORY, "tls_allocate: tls_io region full");
  }
  uint64_t pa = 0;
  Error err = page_allocator_allocate(allocator->pages, 1, &pa);
  if (!error_is_ok(err)) return err;
  err = vmm_map(allocator->vmm, gva, pa, VMM_PAGE_SIZE, VMM_PERM_RW);
  if (!error_is_ok(err)) return err;
  allocator->mapped[allocator->page_count].gva = gva;
  allocator->mapped[allocator->page_count].used_mask = 0;
  allocator->page_count++;
  return OK;
}

Error tls_allocator_init(TLS_Allocator *out, VMM_Context *vmm, Page_Allocator *pages,
                         Address_Region region) {
  if (!out || !vmm || !pages) {
    return ERR(RESULT_INVALID_ARGUMENT, "tls_allocator_init: NULL argument");
  }
  if ((region.base & VMM_PAGE_OFFSET_MASK) != 0 || (region.size & VMM_PAGE_OFFSET_MASK) != 0 ||
      region.size < VMM_PAGE_SIZE) {
    return ERR(RESULT_INVALID_ARGUMENT, "tls_allocator_init: region not page-aligned");
  }
  memset(out, 0, sizeof(*out));
  out->vmm = vmm;
  out->pages = pages;
  out->region = region;
  return OK;
}

Error tls_allocate(TLS_Allocator *allocator, uint64_t *out_gva) {
  if (!allocator || !out_gva || !allocator->vmm) {
    return ERR(RESULT_INVALID_ARGUMENT, "tls_allocate: NULL argument");
  }
  uint32_t block = 0;
  int32_t page = pick_block(allocator, &block);
  if (page == TLS_NO_PAGE) {
    const Error err = map_new_page(allocator);
    if (!error_is_ok(err)) return err;
    page = (int32_t)(allocator->page_count - 1);
    block = 0;
  }
  TLS_Page *slot = &allocator->mapped[page];
  const uint64_t gva = slot->gva + (uint64_t)block * TLS_BLOCK_BYTES;
  const Error err = zero_block(allocator->vmm, gva);
  if (!error_is_ok(err)) return err; /* a freshly mapped page stays for reuse */
  slot->used_mask |= (uint8_t)(1u << block);
  allocator->blocks_in_use++;
  *out_gva = gva;
  return OK;
}

Error tls_free(TLS_Allocator *allocator, uint64_t gva) {
  if (!allocator) return ERR(RESULT_INVALID_ARGUMENT, "tls_free: NULL allocator");
  uint32_t page = 0;
  uint32_t block = 0;
  if (!locate_block(allocator, gva, &page, &block) ||
      (allocator->mapped[page].used_mask & (1u << block)) == 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "tls_free: not an allocated TLS block");
  }
  allocator->mapped[page].used_mask &= (uint8_t)~(1u << block);
  allocator->blocks_in_use--;
  return OK;
}

bool tls_is_allocated(const TLS_Allocator *allocator, uint64_t gva) {
  if (!allocator) return false;
  uint32_t page = 0;
  uint32_t block = 0;
  return locate_block(allocator, gva, &page, &block) &&
         (allocator->mapped[page].used_mask & (1u << block)) != 0;
}

void tls_allocator_teardown(TLS_Allocator *allocator) {
  if (!allocator) return;
  for (uint32_t p = 0; p < allocator->page_count; p++) {
    const Error err = vmm_unmap(allocator->vmm, allocator->mapped[p].gva, VMM_PAGE_SIZE);
    if (!error_is_ok(err)) {
      log_error("tls: unmap of page 0x%llx failed: %s",
                (unsigned long long)allocator->mapped[p].gva, err.message);
    }
  }
  memset(allocator, 0, sizeof(*allocator));
}
