/**
 * core/hle/kernel/page_allocator unit test.
 */
#define CHECK_NAME "page_allocator_test"
#include "check.h"

#include "common/layout.h"
#include "common/vmm.h"
#include "hle/kernel/page_allocator.h"

int main(void) {
  Page_Allocator pages;

  /* Needs a live layout: guest RAM bounds come from it. */
  CHECK_CODE(page_allocator_init_guest_ram(&pages), RESULT_INVALID_ARGUMENT);
  CHECK_OK(layout_create());
  const uint64_t ram = layout_get()->guest_ram_size;

  CHECK_OK(page_allocator_init_guest_ram(&pages));
  CHECK(pages.base_pa == 0 && pages.size_bytes == ram && pages.used_bytes == 0);
  CHECK(page_allocator_free_bytes(&pages) == ram);

  /* Init validation. */
  CHECK_CODE(page_allocator_init(NULL, 0, VMM_PAGE_SIZE), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(page_allocator_init(&pages, 0x10, VMM_PAGE_SIZE), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(page_allocator_init(&pages, 0, VMM_PAGE_SIZE + 1), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(page_allocator_init(&pages, 0, 0), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(page_allocator_init(&pages, ram, VMM_PAGE_SIZE), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(page_allocator_init(&pages, ram - VMM_PAGE_SIZE, 2 * VMM_PAGE_SIZE),
             RESULT_INVALID_ARGUMENT);
  CHECK_OK(page_allocator_init(&pages, ram - VMM_PAGE_SIZE, VMM_PAGE_SIZE));

  /* Bump order over a small window. */
  CHECK_OK(page_allocator_init(&pages, 0x100000, 4 * VMM_PAGE_SIZE));
  uint64_t pa = 0;
  CHECK_OK(page_allocator_allocate(&pages, 1, &pa));
  CHECK(pa == 0x100000);
  CHECK_OK(page_allocator_allocate(&pages, 2, &pa));
  CHECK(pa == 0x100000 + VMM_PAGE_SIZE);
  CHECK(page_allocator_free_bytes(&pages) == VMM_PAGE_SIZE);

  /* Exhaustion leaves the allocator unchanged. */
  CHECK_CODE(page_allocator_allocate(&pages, 2, &pa), RESULT_OUT_OF_MEMORY);
  CHECK(pages.used_bytes == 3 * VMM_PAGE_SIZE);
  /* A count whose byte size would overflow is still just "too many". */
  CHECK_CODE(page_allocator_allocate(&pages, UINT64_MAX, &pa), RESULT_OUT_OF_MEMORY);
  CHECK_OK(page_allocator_allocate(&pages, 1, &pa));
  CHECK(pa == 0x100000 + 3 * VMM_PAGE_SIZE);
  CHECK(page_allocator_free_bytes(&pages) == 0);

  CHECK_CODE(page_allocator_allocate(&pages, 0, &pa), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(page_allocator_allocate(&pages, 1, NULL), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(page_allocator_allocate(NULL, 1, &pa), RESULT_INVALID_ARGUMENT);

  page_allocator_reset(&pages);
  CHECK(pages.used_bytes == 0);
  CHECK_OK(page_allocator_allocate(&pages, 4, &pa));
  CHECK(pa == 0x100000);

  layout_destroy();
  return 0;
}
