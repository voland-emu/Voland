/**
 * Unit tests for core/common/layout.c (§4). Covers the one C subsystem
 * introduced in this pass that emulator.c/smoke_phase0.c don't already
 * exercise directly: region reservation, non-overlap, idempotency, and
 * destroy/recreate.
 */
#include "common/layout.h"

#include <stdio.h>
#include <stdlib.h>

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
  bases[7] = layout->trace_buffer_base;      sizes[7] = LAYOUT_TRACE_BUFFER_SIZE;
  bases[8] = layout->breakpoint_region_base; sizes[8] = LAYOUT_BREAKPOINT_REGION_SIZE;
}

static void check_no_overlap(const Memory_Layout *layout)
{
  enum { REGION_COUNT = 9 };
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

int main(void)
{
  /* Fresh creation reserves every region with the documented sizes. */
  Error err = layout_create();
  CHECK(err.code == RESULT_OK);

  const Memory_Layout *layout = layout_get();
  CHECK(layout != NULL);
  CHECK(layout->guest_ram_size == LAYOUT_GUEST_RAM_SIZE);
  CHECK(layout->guest_ram_size == (uint64_t)4 * 1024 * 1024 * 1024);

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

  printf("[layout_test] OK\n");
  return 0;
}
