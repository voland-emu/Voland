/**
 * Unit tests for core/common/layout.c (§4). Covers the one C subsystem
 * introduced in this pass that emulator.c/smoke_phase0.c don't already
 * exercise directly: region reservation, non-overlap, idempotency,
 * destroy/recreate, the wasm32 guest RAM sizes, and committing guest RAM
 * (reserved without committing on native, §4).
 */
#include "common/layout.h"
#include "common/vmm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define CHECK(cond)                                                   \
  do                                                                  \
  {                                                                   \
    if (!(cond))                                                      \
    {                                                                 \
      fprintf(stderr, "[layout_test] FAIL %s:%d: %s\n", __FILE__,     \
              __LINE__, #cond);                                       \
      exit(1);                                                        \
    }                                                                 \
  } while (0)

/* Every region base as a flat array, for the overlap/order checks below.
 * Order matches layout_create()'s allocation order (arena-packed, so
 * ranges are contiguous with zero gap once alignment padding is
 * accounted for - see the exact-adjacency assertions further down). */
static void collect_bases_and_sizes(const Memory_Layout *layout,
                                     uint64_t *bases, uint64_t *sizes, size_t count)
{
  (void)count;
  bases[0] = layout->guest_ram_base;         sizes[0] = layout->guest_ram_size;
  bases[1] = layout->page_table_l1_base;     sizes[1] = LAYOUT_PAGE_TABLE_L1_SIZE;
  bases[2] = layout->framebuffer_slot_base;  sizes[2] = LAYOUT_FRAMEBUFFER_REGION_SIZE;
  bases[3] = layout->audio_ring_base;        sizes[3] = LAYOUT_AUDIO_RING_SIZE;
  bases[4] = layout->gpu_ring_base;          sizes[4] = LAYOUT_GPU_RING_SIZE;
  bases[5] = layout->gpu_completion_ring_base; sizes[5] = LAYOUT_GPU_COMPLETION_RING_SIZE;
  bases[6] = layout->input_region_base;      sizes[6] = LAYOUT_INPUT_REGION_SIZE;
  bases[7] = layout->output_region_base;     sizes[7] = LAYOUT_OUTPUT_REGION_SIZE;
  bases[8] = layout->trace_buffer_base;      sizes[8] = LAYOUT_TRACE_BUFFER_SIZE;
  bases[9] = layout->breakpoint_region_base; sizes[9] = LAYOUT_BREAKPOINT_REGION_SIZE;
}

static void check_no_overlap(const Memory_Layout *layout)
{
  enum { REGION_COUNT = 10 };
  uint64_t bases[REGION_COUNT];
  uint64_t sizes[REGION_COUNT];
  collect_bases_and_sizes(layout, bases, sizes, REGION_COUNT);

  for (size_t i = 0; i < REGION_COUNT; i++)
  {
    CHECK(bases[i] != 0);
    for (size_t j = i + 1; j < REGION_COUNT; j++)
    {
      const uint64_t a_start = bases[i], a_end = bases[i] + sizes[i];
      const uint64_t b_start = bases[j], b_end = bases[j] + sizes[j];
      CHECK(a_end <= b_start || b_end <= a_start);
    }
  }
}

static uint8_t *guest_ram_host(uint64_t guest_pa)
{
  return (uint8_t *)(uintptr_t)(layout_get()->guest_ram_base + guest_pa);
}

#if defined(_WIN32)
/* VirtualQuery state (MEM_RESERVE / MEM_COMMIT) of one guest RAM page. */
static DWORD page_state(uint64_t guest_pa)
{
  MEMORY_BASIC_INFORMATION info;
  CHECK(VirtualQuery(guest_ram_host(guest_pa), &info, sizeof(info)) == sizeof(info));
  return info.State;
}
#endif

/* Commits one page, then proves it is zero on first commit and writable. */
static void check_commit_page(uint64_t guest_pa)
{
#if defined(_WIN32)
  CHECK(page_state(guest_pa) == MEM_RESERVE);
#endif
  CHECK(layout_guest_ram_commit(guest_pa, VMM_PAGE_SIZE).code == RESULT_OK);
#if defined(_WIN32)
  CHECK(page_state(guest_pa) == MEM_COMMIT);
#endif
  uint8_t *page = guest_ram_host(guest_pa);
  for (uint64_t i = 0; i < VMM_PAGE_SIZE; i++)
    CHECK(page[i] == 0);
  memset(page, 0x5A, (size_t)VMM_PAGE_SIZE);
  CHECK(page[0] == 0x5A && page[VMM_PAGE_SIZE - 1] == 0x5A);
  /* Committing again is a no-op: the bytes survive. */
  CHECK(layout_guest_ram_commit(guest_pa, VMM_PAGE_SIZE).code == RESULT_OK);
  CHECK(page[0] == 0x5A);
}

static void test_guest_ram_commit(void)
{
  /* No live layout. */
  CHECK(layout_guest_ram_commit(0, VMM_PAGE_SIZE).code == RESULT_INVALID_ARGUMENT);

  CHECK(layout_create().code == RESULT_OK);
  const uint64_t ram = layout_get()->guest_ram_size;
  CHECK((layout_get()->guest_ram_base & VMM_PAGE_OFFSET_MASK) == 0);

  CHECK(layout_guest_ram_commit(0, 0).code == RESULT_INVALID_ARGUMENT);
  CHECK(layout_guest_ram_commit(8, VMM_PAGE_SIZE).code == RESULT_INVALID_ARGUMENT);
  CHECK(layout_guest_ram_commit(0, VMM_PAGE_SIZE + 1).code == RESULT_INVALID_ARGUMENT);
  CHECK(layout_guest_ram_commit(ram, VMM_PAGE_SIZE).code == RESULT_INVALID_ARGUMENT);
  CHECK(layout_guest_ram_commit(ram - VMM_PAGE_SIZE, 2 * VMM_PAGE_SIZE).code ==
        RESULT_INVALID_ARGUMENT);
  /* pa + size wraps: still outside, not a tiny range. */
  CHECK(layout_guest_ram_commit(VMM_PAGE_SIZE, UINT64_MAX - VMM_PAGE_OFFSET_MASK).code ==
        RESULT_INVALID_ARGUMENT);

#if defined(_WIN32)
  /* Reserved, not committed (§4): a fresh layout charges nothing against
   * the commit limit. */
  CHECK(page_state(0) == MEM_RESERVE);
  CHECK(page_state(ram / 2) == MEM_RESERVE);
  CHECK(page_state(ram - VMM_PAGE_SIZE) == MEM_RESERVE);
#endif

  check_commit_page(0);
  check_commit_page(LAYOUT_GUEST_RAM_HLE_CARVEOUT_BASE_PA);
  check_commit_page(ram - VMM_PAGE_SIZE);

#if defined(_WIN32)
  /* Exactly the committed ranges changed state; their neighbors didn't. */
  CHECK(page_state(VMM_PAGE_SIZE) == MEM_RESERVE);
  CHECK(page_state(LAYOUT_GUEST_RAM_HLE_CARVEOUT_BASE_PA - VMM_PAGE_SIZE) == MEM_RESERVE);
  CHECK(page_state(ram - 2 * VMM_PAGE_SIZE) == MEM_RESERVE);
#endif

  layout_destroy();
}

int main(void)
{
  /* wasm32 sizing (§4): the application pool plus the HLE carve-out,
   * which starts where the pool ends; all page multiples. */
  CHECK(LAYOUT_GUEST_RAM_APPLICATION_POOL_SIZE == (uint64_t)3285 * 1024 * 1024);
  CHECK(LAYOUT_GUEST_RAM_HLE_CARVEOUT_SIZE == (uint64_t)48 * 1024 * 1024);
  CHECK(LAYOUT_GUEST_RAM_HLE_CARVEOUT_BASE_PA == LAYOUT_GUEST_RAM_APPLICATION_POOL_SIZE);
  CHECK(LAYOUT_GUEST_RAM_SIZE == (uint64_t)3333 * 1024 * 1024);
  CHECK((LAYOUT_GUEST_RAM_SIZE & VMM_PAGE_OFFSET_MASK) == 0);
  CHECK((LAYOUT_GUEST_RAM_HLE_CARVEOUT_BASE_PA & VMM_PAGE_OFFSET_MASK) == 0);
  /* 65535 pages, never 65536 (nothing ends at 2^32, §4). */
  CHECK(LAYOUT_LINEAR_MEMORY_BYTES == (uint64_t)4294901760);

  /* Input region (§18, v3.29): 8 x 64-byte controller slots + one 128-byte
   * touch block = 640 bytes. Output region: 8 x 32-byte slots = 256 bytes,
   * reserved only (no writer/reader until Phase 4). */
  CHECK(LAYOUT_INPUT_REGION_SLOT_BYTES == 64);
  CHECK(LAYOUT_INPUT_TOUCH_BLOCK_BYTES == 128);
  CHECK(LAYOUT_INPUT_REGION_SIZE == 640);
  CHECK(LAYOUT_OUTPUT_REGION_SIZE == 256);


  /* Fresh creation reserves every region with the documented sizes. */
  Error err = layout_create();
  CHECK(err.code == RESULT_OK);

  const Memory_Layout *layout = layout_get();
  CHECK(layout != NULL);
  CHECK(layout->guest_ram_size == LAYOUT_GUEST_RAM_SIZE);
  CHECK(layout->guest_ram_size == (uint64_t)3333 * 1024 * 1024);

  check_no_overlap(layout);

  /* Idempotent: a second create call while live is a no-op that returns
   * the same layout, not a fresh (and leaked) allocation. */
  const uint64_t guest_ram_base_before = layout->guest_ram_base;
  err = layout_create();
  CHECK(err.code == RESULT_OK);
  CHECK(layout_get()->guest_ram_base == guest_ram_base_before);

  /* Destroy releases everything; layout_get() reports no live layout. */
  layout_destroy();
  CHECK(layout_get() == NULL);

  /* Destroying an already-destroyed layout is safe. */
  layout_destroy();
  CHECK(layout_get() == NULL);

  /* Recreation after destroy works and produces a valid layout again
   * (not necessarily at the same addresses). */
  err = layout_create();
  CHECK(err.code == RESULT_OK);
  layout = layout_get();
  CHECK(layout != NULL);
  check_no_overlap(layout);

  layout_destroy();

  test_guest_ram_commit();

  printf("[layout_test] OK\n");
  return 0;
}
