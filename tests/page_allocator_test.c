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

  /* --- Release: cursor retraction, coalescing, first-fit reuse, and
   * split-on-allocate. `pages` still has all 4 pages from `pa` bumped. --- */

  /* The last page is adjacent to the cursor: retracts it directly, no
   * freelist entry needed. */
  CHECK_OK(page_allocator_free(&pages, pa + 3 * VMM_PAGE_SIZE, 1));
  CHECK(pages.used_bytes == 3 * VMM_PAGE_SIZE);
  CHECK(pages.free_run_count == 0);

  /* Page 1 is not adjacent to the (now retracted) cursor - page 2 still
   * sits between it and the cursor - so it goes to the freelist. */
  CHECK_OK(page_allocator_free(&pages, pa + VMM_PAGE_SIZE, 1));
  CHECK(pages.free_run_count == 1);
  CHECK(pages.free_runs[0].pa == pa + VMM_PAGE_SIZE && pages.free_runs[0].page_count == 1);

  /* Page 0 is directly adjacent to that freelist entry: coalesces into
   * it rather than adding a second one. */
  CHECK_OK(page_allocator_free(&pages, pa, 1));
  CHECK(pages.free_run_count == 1);
  CHECK(pages.free_runs[0].pa == pa && pages.free_runs[0].page_count == 2);
  CHECK(page_allocator_free_bytes(&pages) == VMM_PAGE_SIZE /* cursor slack */ + 2 * VMM_PAGE_SIZE);

  /* An exact-size allocation consumes the freelist run whole and does
   * NOT move the cursor - it came from release, not bump. */
  uint64_t reused_pa = 0;
  CHECK_OK(page_allocator_allocate(&pages, 2, &reused_pa));
  CHECK(reused_pa == pa);
  CHECK(pages.free_run_count == 0);
  CHECK(pages.used_bytes == 3 * VMM_PAGE_SIZE);

  /* Freeing pages 0+1 again (still not cursor-adjacent - page 2 is
   * untouched and still sits below the cursor) then asking for only one
   * of them back splits the freelist run, leaving the remainder in it. */
  CHECK_OK(page_allocator_free(&pages, pa, 2));
  uint64_t split_pa = 0;
  CHECK_OK(page_allocator_allocate(&pages, 1, &split_pa));
  CHECK(split_pa == pa);
  CHECK(pages.free_run_count == 1);
  CHECK(pages.free_runs[0].pa == pa + VMM_PAGE_SIZE && pages.free_runs[0].page_count == 1);

  /* Argument validation, and a range this allocator never handed out. */
  CHECK_CODE(page_allocator_free(NULL, pa, 1), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(page_allocator_free(&pages, pa, 0), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(page_allocator_free(&pages, pa + 1, 1), RESULT_INVALID_ARGUMENT); /* misaligned */
  CHECK_CODE(page_allocator_free(&pages, pa - VMM_PAGE_SIZE, 1), RESULT_INVALID_ARGUMENT); /* before base_pa */
  CHECK_CODE(page_allocator_free(&pages, pa + 4 * VMM_PAGE_SIZE, 1), RESULT_INVALID_ARGUMENT); /* past used_bytes */

  /* --- Freelist exhaustion: PAGE_ALLOCATOR_MAX_FREE_RUNS non-adjacent
   * runs fill it (odd-indexed pages stay allocated as spacers, and the
   * top page stays allocated so the cursor never retracts); one more is
   * rejected and changes nothing. --- */
  Page_Allocator spread;
  CHECK_OK(page_allocator_init(&spread, 0x200000, 34 * VMM_PAGE_SIZE));
  uint64_t spread_pa = 0;
  CHECK_OK(page_allocator_allocate(&spread, 34, &spread_pa));
  for (uint32_t i = 0; i < PAGE_ALLOCATOR_MAX_FREE_RUNS; i++) {
    CHECK_OK(page_allocator_free(&spread, spread_pa + (uint64_t)(2 * i) * VMM_PAGE_SIZE, 1));
  }
  CHECK(spread.free_run_count == PAGE_ALLOCATOR_MAX_FREE_RUNS);
  CHECK_CODE(
      page_allocator_free(&spread, spread_pa + (uint64_t)(2 * PAGE_ALLOCATOR_MAX_FREE_RUNS) * VMM_PAGE_SIZE, 1),
      RESULT_OUT_OF_MEMORY);
  CHECK(spread.free_run_count == PAGE_ALLOCATOR_MAX_FREE_RUNS);

  layout_destroy();
  return 0;
}
