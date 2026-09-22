/**
 * core/hle/kernel/tls unit test: the per-process TLS block allocator
 * (§12 "TLS is the transport") over a real vmm and page allocator.
 * Everything guest-visible is asserted through vmm_query /
 * vmm_read_block / vmm_write_block, never a raw pointer.
 */
#define CHECK_NAME "tls_test"
#include "check.h"

#include "common/layout.h"
#include "common/vmm.h"
#include "hle/kernel/tls.h"

#include <string.h>

#define PAGE VMM_PAGE_SIZE
#define REGION_BASE ((uint64_t)0x10000000)

static void expect_mapped_rw(VMM_Context *vmm, uint64_t gva) {
  VMM_Region_Info info;
  CHECK_OK(vmm_query(vmm, gva, &info));
  CHECK(info.is_mapped && info.perms == VMM_PERM_RW);
}

static void expect_unmapped(VMM_Context *vmm, uint64_t gva) {
  VMM_Region_Info info;
  CHECK_OK(vmm_query(vmm, gva, &info));
  CHECK(!info.is_mapped);
}

static bool block_is_zero(VMM_Context *vmm, uint64_t gva) {
  uint8_t bytes[TLS_BLOCK_BYTES];
  CHECK_OK(vmm_read_block(vmm, gva, bytes, TLS_BLOCK_BYTES));
  for (size_t i = 0; i < sizeof(bytes); i++) {
    if (bytes[i] != 0) return false;
  }
  return true;
}

static void test_init_validation(VMM_Context *vmm, Page_Allocator *pages) {
  TLS_Allocator tls;
  const Address_Region good = {REGION_BASE, 4 * PAGE};
  const Address_Region unaligned_base = {REGION_BASE + 0x10, PAGE};
  const Address_Region unaligned_size = {REGION_BASE, PAGE + 1};
  const Address_Region empty = {REGION_BASE, 0};
  CHECK_CODE(tls_allocator_init(NULL, vmm, pages, good), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(tls_allocator_init(&tls, NULL, pages, good), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(tls_allocator_init(&tls, vmm, NULL, good), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(tls_allocator_init(&tls, vmm, pages, unaligned_base), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(tls_allocator_init(&tls, vmm, pages, unaligned_size), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(tls_allocator_init(&tls, vmm, pages, empty), RESULT_INVALID_ARGUMENT);
  CHECK_OK(tls_allocator_init(&tls, vmm, pages, good));
  CHECK(tls.page_count == 0 && tls.blocks_in_use == 0);
  expect_unmapped(vmm, REGION_BASE); /* init maps nothing */
}

static void test_allocation_order(VMM_Context *vmm, Page_Allocator *pages) {
  TLS_Allocator tls;
  const Address_Region region = {REGION_BASE, 4 * PAGE};
  CHECK_OK(tls_allocator_init(&tls, vmm, pages, region));

  uint64_t gva = 0;
  CHECK_CODE(tls_allocate(&tls, NULL), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(tls_allocate(NULL, &gva), RESULT_INVALID_ARGUMENT);

  /* The first page is mapped by the first allocation, and only then. */
  CHECK_OK(tls_allocate(&tls, &gva));
  CHECK(gva == REGION_BASE);
  CHECK(tls.page_count == 1 && tls.blocks_in_use == 1);
  expect_mapped_rw(vmm, REGION_BASE);
  expect_unmapped(vmm, REGION_BASE + PAGE);
  CHECK(tls_is_allocated(&tls, gva));
  CHECK(!tls_is_allocated(&tls, gva + TLS_BLOCK_BYTES));

  /* Blocks 1..7 fill the same page in address order. */
  for (uint32_t b = 1; b < TLS_BLOCKS_PER_PAGE; b++) {
    CHECK_OK(tls_allocate(&tls, &gva));
    CHECK(gva == REGION_BASE + b * TLS_BLOCK_BYTES);
    CHECK(tls.page_count == 1);
  }
  CHECK(tls.blocks_in_use == TLS_BLOCKS_PER_PAGE);
  expect_unmapped(vmm, REGION_BASE + PAGE);

  /* The ninth maps a second page. */
  CHECK_OK(tls_allocate(&tls, &gva));
  CHECK(gva == REGION_BASE + PAGE);
  CHECK(tls.page_count == 2 && tls.blocks_in_use == TLS_BLOCKS_PER_PAGE + 1);
  expect_mapped_rw(vmm, REGION_BASE + PAGE);

  /* Free a mid-page block on page 0: neighbours untouched, and the next
   * allocation reuses it (lowest page, lowest block) instead of page 1. */
  const uint64_t mid = REGION_BASE + 3 * TLS_BLOCK_BYTES;
  CHECK_OK(tls_free(&tls, mid));
  CHECK(!tls_is_allocated(&tls, mid));
  CHECK(tls_is_allocated(&tls, mid - TLS_BLOCK_BYTES));
  CHECK(tls_is_allocated(&tls, mid + TLS_BLOCK_BYTES));
  CHECK(tls.blocks_in_use == TLS_BLOCKS_PER_PAGE);
  expect_mapped_rw(vmm, REGION_BASE); /* the page stays mapped */
  CHECK_OK(tls_allocate(&tls, &gva));
  CHECK(gva == mid);
  CHECK(tls.page_count == 2);

  /* Free two blocks on different pages: the lower page wins. */
  CHECK_OK(tls_free(&tls, REGION_BASE + PAGE));
  CHECK_OK(tls_free(&tls, REGION_BASE + 6 * TLS_BLOCK_BYTES));
  CHECK_OK(tls_allocate(&tls, &gva));
  CHECK(gva == REGION_BASE + 6 * TLS_BLOCK_BYTES);
  CHECK_OK(tls_allocate(&tls, &gva));
  CHECK(gva == REGION_BASE + PAGE);

  /* Rejected frees leave the pool as it is. */
  const uint32_t before = tls.blocks_in_use;
  CHECK_CODE(tls_free(&tls, REGION_BASE + 1), RESULT_INVALID_ARGUMENT);        /* unaligned */
  CHECK_CODE(tls_free(&tls, REGION_BASE + 2 * PAGE), RESULT_INVALID_ARGUMENT); /* unmapped page */
  CHECK_CODE(tls_free(&tls, REGION_BASE - PAGE), RESULT_INVALID_ARGUMENT);     /* outside */
  CHECK_CODE(tls_free(&tls, REGION_BASE + PAGE + TLS_BLOCK_BYTES),
             RESULT_INVALID_ARGUMENT);                                          /* never allocated */
  CHECK_OK(tls_free(&tls, REGION_BASE));
  CHECK_CODE(tls_free(&tls, REGION_BASE), RESULT_INVALID_ARGUMENT); /* double free */
  CHECK_CODE(tls_free(NULL, REGION_BASE), RESULT_INVALID_ARGUMENT);
  CHECK(tls.blocks_in_use == before - 1);
  CHECK(!tls_is_allocated(NULL, REGION_BASE));
  CHECK(!tls_is_allocated(&tls, REGION_BASE + 1));

  /* Teardown unmaps every page; safe to repeat on the zeroed result. */
  tls_allocator_teardown(&tls);
  CHECK(tls.page_count == 0 && tls.blocks_in_use == 0 && tls.vmm == NULL);
  expect_unmapped(vmm, REGION_BASE);
  expect_unmapped(vmm, REGION_BASE + PAGE);
  tls_allocator_teardown(&tls);
}

static void test_blocks_are_zeroed(VMM_Context *vmm, Page_Allocator *pages) {
  /* Dirty the physical page the allocator will receive next: map it
   * somewhere else, scribble through vmm, unmap, and rewind the page
   * allocator so the same page comes back. */
  const uint64_t used_before = pages->used_bytes;
  uint64_t pa = 0;
  CHECK_OK(page_allocator_allocate(pages, 1, &pa));
  const uint64_t scratch_gva = REGION_BASE + 16 * PAGE;
  CHECK_OK(vmm_map(vmm, scratch_gva, pa, PAGE, VMM_PERM_RW));
  uint8_t junk[PAGE];
  memset(junk, 0xA5, sizeof(junk));
  CHECK_OK(vmm_write_block(vmm, scratch_gva, junk, PAGE));
  CHECK_OK(vmm_unmap(vmm, scratch_gva, PAGE));
  pages->used_bytes = used_before;

  TLS_Allocator tls;
  const Address_Region region = {REGION_BASE, 4 * PAGE};
  CHECK_OK(tls_allocator_init(&tls, vmm, pages, region));
  uint64_t gva = 0;
  CHECK_OK(tls_allocate(&tls, &gva));
  CHECK(block_is_zero(vmm, gva));
  /* Only the allocated block was cleaned: block 1 still carries the
   * junk, and is cleaned when it is handed out. */
  CHECK(!block_is_zero(vmm, gva + TLS_BLOCK_BYTES));
  CHECK_OK(tls_allocate(&tls, &gva));
  CHECK(gva == REGION_BASE + TLS_BLOCK_BYTES);
  CHECK(block_is_zero(vmm, gva));

  /* A freed block is not zeroed until reallocation. */
  CHECK_OK(vmm_write_block(vmm, gva, junk, TLS_BLOCK_BYTES));
  CHECK_OK(tls_free(&tls, gva));
  CHECK(!block_is_zero(vmm, gva));
  CHECK_OK(tls_allocate(&tls, &gva));
  CHECK(gva == REGION_BASE + TLS_BLOCK_BYTES);
  CHECK(block_is_zero(vmm, gva));
  tls_allocator_teardown(&tls);
}

static void test_exhaustion(VMM_Context *vmm, Page_Allocator *pages) {
  /* Region smaller than TLS_MAX_PAGES: region-full is the limit hit. */
  TLS_Allocator tls;
  const Address_Region small = {REGION_BASE, 2 * PAGE};
  CHECK_OK(tls_allocator_init(&tls, vmm, pages, small));
  uint64_t gva = 0;
  for (uint32_t i = 0; i < 2 * TLS_BLOCKS_PER_PAGE; i++) CHECK_OK(tls_allocate(&tls, &gva));
  CHECK_CODE(tls_allocate(&tls, &gva), RESULT_OUT_OF_MEMORY);
  CHECK(tls.page_count == 2 && tls.blocks_in_use == 2 * TLS_BLOCKS_PER_PAGE);
  expect_unmapped(vmm, REGION_BASE + 2 * PAGE);
  tls_allocator_teardown(&tls);

  /* The page cap: TLS_MAX_BLOCKS succeed, one more fails cleanly. */
  const Address_Region wide = {REGION_BASE, (uint64_t)(TLS_MAX_PAGES + 4) * PAGE};
  CHECK_OK(tls_allocator_init(&tls, vmm, pages, wide));
  for (uint32_t i = 0; i < TLS_MAX_BLOCKS; i++) {
    CHECK_OK(tls_allocate(&tls, &gva));
    CHECK(gva == REGION_BASE + (i / TLS_BLOCKS_PER_PAGE) * PAGE +
                     (i % TLS_BLOCKS_PER_PAGE) * TLS_BLOCK_BYTES);
  }
  CHECK(tls.page_count == TLS_MAX_PAGES && tls.blocks_in_use == TLS_MAX_BLOCKS);
  CHECK_CODE(tls_allocate(&tls, &gva), RESULT_OUT_OF_MEMORY);
  CHECK(tls.page_count == TLS_MAX_PAGES && tls.blocks_in_use == TLS_MAX_BLOCKS);
  expect_unmapped(vmm, REGION_BASE + (uint64_t)TLS_MAX_PAGES * PAGE);
  /* Freeing anywhere reopens exactly one slot. */
  CHECK_OK(tls_free(&tls, REGION_BASE + 17 * PAGE + 5 * TLS_BLOCK_BYTES));
  CHECK_OK(tls_allocate(&tls, &gva));
  CHECK(gva == REGION_BASE + 17 * PAGE + 5 * TLS_BLOCK_BYTES);
  CHECK_CODE(tls_allocate(&tls, &gva), RESULT_OUT_OF_MEMORY);
  tls_allocator_teardown(&tls);
  for (uint32_t p = 0; p < TLS_MAX_PAGES; p++) expect_unmapped(vmm, REGION_BASE + p * PAGE);

  /* Physical pages exhausted: the page boundary fails with nothing half
   * mapped and the allocator unchanged. */
  Page_Allocator one_page;
  CHECK_OK(page_allocator_init(&one_page, 0x400000, PAGE));
  CHECK_OK(tls_allocator_init(&tls, vmm, &one_page, wide));
  for (uint32_t i = 0; i < TLS_BLOCKS_PER_PAGE; i++) CHECK_OK(tls_allocate(&tls, &gva));
  CHECK_CODE(tls_allocate(&tls, &gva), RESULT_OUT_OF_MEMORY);
  CHECK(tls.page_count == 1 && tls.blocks_in_use == TLS_BLOCKS_PER_PAGE);
  expect_unmapped(vmm, REGION_BASE + PAGE);
  tls_allocator_teardown(&tls);
}

int main(void) {
  CHECK_OK(layout_create());
  VMM_Context *vmm = vmm_create();
  CHECK(vmm != NULL);
  Page_Allocator pages;
  CHECK_OK(page_allocator_init(&pages, 0, 256 * PAGE));

  test_init_validation(vmm, &pages);
  test_allocation_order(vmm, &pages);
  test_blocks_are_zeroed(vmm, &pages);
  test_exhaustion(vmm, &pages);

  vmm_destroy(vmm);
  layout_destroy();
  printf("[" CHECK_NAME "] ok\n");
  return 0;
}
