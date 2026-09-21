/**
 * Unit tests for core/common/vmm.c - the softmmu (§5).
 *
 * Covers lifecycle, argument validation, map/unmap/reprotect/query
 * semantics (validate-then-commit, mirrors, PERM_NONE pages, run
 * merging), the checked and inline access paths (including cross-page
 * all-or-nothing accesses), block copies over scattered backing,
 * guest_to_host contiguity, L2 table exhaustion + recycling, and the
 * debug borrow scopes.
 *
 * This file is the one place that legitimately peeks at guest PHYSICAL
 * RAM (layout guest_ram_base + pa): proving that a virtual write landed
 * on the right physical page is the whole point of the test.
 */
#include "common/layout.h"
#include "common/vmm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                       \
  do                                                                      \
  {                                                                       \
    if (!(cond))                                                          \
    {                                                                     \
      fprintf(stderr, "[vmm_test] FAIL %s:%d: %s\n", __FILE__, __LINE__, \
              #cond);                                                     \
      exit(1);                                                            \
    }                                                                     \
  } while (0)

#define CHECK_OK(expr) CHECK((expr).code == RESULT_OK)
#define CHECK_CODE(expr, expected) CHECK((expr).code == (expected))

/* Test addresses. GVA_A sits in L1 slot 2 (128MB); GVA_FAR sits in slot
 * 4000 (~250GB), far enough away to prove sparse slabs work. */
#define GVA_A ((uint64_t)0x08000000)
#define GVA_FAR ((uint64_t)4000 * VMM_L2_SPAN_BYTES)
#define PA_A ((uint64_t)0x1000)
#define PA_B ((uint64_t)0x00900000) /* 9MB, deliberately far from PA_A */
#define LAST_PAGE_GVA (VMM_ADDRESS_SPACE_SIZE - VMM_PAGE_SIZE)

static uint8_t *physical(uint64_t guest_pa)
{
  return (uint8_t *)(uintptr_t)(layout_get()->guest_ram_base + guest_pa);
}

static void expect_query(VMM_Context *vmm, uint64_t gva, uint64_t base, uint64_t size,
                         bool mapped, uint32_t perms)
{
  VMM_Region_Info info;
  CHECK_OK(vmm_query(vmm, gva, &info));
  if (info.base_gva != base || info.size != size || info.is_mapped != mapped || info.perms != perms)
  {
    fprintf(stderr, "[vmm_test] query(0x%llx): got base=0x%llx size=0x%llx mapped=%d perms=%u, "
                    "want base=0x%llx size=0x%llx mapped=%d perms=%u\n",
            (unsigned long long)gva, (unsigned long long)info.base_gva,
            (unsigned long long)info.size, (int)info.is_mapped, info.perms,
            (unsigned long long)base, (unsigned long long)size, (int)mapped, perms);
  }
  CHECK(info.base_gva == base);
  CHECK(info.size == size);
  CHECK(info.is_mapped == mapped);
  CHECK(info.perms == perms);
}

/* ---------------------------------------------------------------- */

static void test_lifecycle(void)
{
  /* No layout, no vmm. */
  CHECK(layout_get() == NULL);
  CHECK(vmm_create() == NULL);

  CHECK_OK(layout_create());
  const Memory_Layout *layout = layout_get();

  /* Dirty the L1 region to prove vmm_create zeroes it. */
  memset((void *)(uintptr_t)layout->page_table_l1_base, 0xAB, (size_t)LAYOUT_PAGE_TABLE_L1_SIZE);

  VMM_Context *vmm = vmm_create();
  CHECK(vmm != NULL);
  const uint64_t *l1 = vmm_page_table_l1(vmm);
  CHECK((uint64_t)(uintptr_t)l1 == layout->page_table_l1_base);
  for (uint64_t i = 0; i < VMM_L1_ENTRY_COUNT; i++)
    CHECK(l1[i] == 0);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_NONE);

  /* One live context at a time. */
  CHECK(vmm_create() == NULL);

  /* Destroy (NULL-safe), recreate. */
  vmm_destroy(NULL);
  vmm_destroy(vmm);
  vmm = vmm_create();
  CHECK(vmm != NULL);
  vmm_destroy(vmm);
  layout_destroy();
}

static void test_map_validation(VMM_Context *vmm)
{
  const uint64_t ram = layout_get()->guest_ram_size;
  CHECK_CODE(vmm_map(NULL, GVA_A, PA_A, VMM_PAGE_SIZE, VMM_PERM_RW), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_map(vmm, GVA_A + 1, PA_A, VMM_PAGE_SIZE, VMM_PERM_RW), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_map(vmm, GVA_A, PA_A + 8, VMM_PAGE_SIZE, VMM_PERM_RW), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_map(vmm, GVA_A, PA_A, VMM_PAGE_SIZE + 1, VMM_PERM_RW), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_map(vmm, GVA_A, PA_A, 0, VMM_PERM_RW), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_map(vmm, LAST_PAGE_GVA, PA_A, 2 * VMM_PAGE_SIZE, VMM_PERM_RW), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_map(vmm, VMM_ADDRESS_SPACE_SIZE, PA_A, VMM_PAGE_SIZE, VMM_PERM_RW), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_map(vmm, GVA_A, ram, VMM_PAGE_SIZE, VMM_PERM_RW), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_map(vmm, GVA_A, ram - VMM_PAGE_SIZE, 2 * VMM_PAGE_SIZE, VMM_PERM_RW), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_map(vmm, GVA_A, PA_A, VMM_PAGE_SIZE, 0x8), RESULT_INVALID_ARGUMENT);

  /* Overlap with an existing mapping: whole call rejected, nothing changes. */
  CHECK_OK(vmm_map(vmm, GVA_A, PA_A, VMM_PAGE_SIZE, VMM_PERM_RW));
  CHECK_CODE(vmm_map(vmm, GVA_A - VMM_PAGE_SIZE, PA_B, 3 * VMM_PAGE_SIZE, VMM_PERM_R), RESULT_INVALID_ARGUMENT);
  expect_query(vmm, GVA_A - VMM_PAGE_SIZE, 0, GVA_A, false, VMM_PERM_NONE);
  expect_query(vmm, GVA_A, GVA_A, VMM_PAGE_SIZE, true, VMM_PERM_RW);
  expect_query(vmm, GVA_A + VMM_PAGE_SIZE, GVA_A + VMM_PAGE_SIZE,
               VMM_ADDRESS_SPACE_SIZE - (GVA_A + VMM_PAGE_SIZE), false, VMM_PERM_NONE);
  CHECK_OK(vmm_unmap(vmm, GVA_A, VMM_PAGE_SIZE));
}

static void test_round_trip_and_physical_backing(VMM_Context *vmm)
{
  CHECK_OK(vmm_map(vmm, GVA_A, PA_A, 3 * VMM_PAGE_SIZE, VMM_PERM_RW));

  CHECK_OK(vmm_write8(vmm, GVA_A + 0x10, 0xA5));
  CHECK_OK(vmm_write16(vmm, GVA_A + 0x20, 0xBEEF));
  CHECK_OK(vmm_write32(vmm, GVA_A + 0x30, 0xDEADBEEFu));
  CHECK_OK(vmm_write64(vmm, GVA_A + 2 * VMM_PAGE_SIZE + 0x40, 0x0123456789ABCDEFull));

  uint8_t v8 = 0;
  uint16_t v16 = 0;
  uint32_t v32 = 0;
  uint64_t v64 = 0;
  CHECK_OK(vmm_read8(vmm, GVA_A + 0x10, &v8));
  CHECK_OK(vmm_read16(vmm, GVA_A + 0x20, &v16));
  CHECK_OK(vmm_read32(vmm, GVA_A + 0x30, &v32));
  CHECK_OK(vmm_read64(vmm, GVA_A + 2 * VMM_PAGE_SIZE + 0x40, &v64));
  CHECK(v8 == 0xA5);
  CHECK(v16 == 0xBEEF);
  CHECK(v32 == 0xDEADBEEFu);
  CHECK(v64 == 0x0123456789ABCDEFull);

  /* The bytes landed on the physical pages the mapping named, LE. */
  CHECK(physical(PA_A)[0x10] == 0xA5);
  CHECK(physical(PA_A)[0x30] == 0xEF);
  CHECK(physical(PA_A)[0x33] == 0xDE);
  CHECK(physical(PA_A + 2 * VMM_PAGE_SIZE)[0x40] == 0xEF);
  CHECK(physical(PA_A + 2 * VMM_PAGE_SIZE)[0x47] == 0x01);

  CHECK_OK(vmm_unmap(vmm, GVA_A, 3 * VMM_PAGE_SIZE));
}

static void test_mirrors(VMM_Context *vmm)
{
  CHECK_OK(vmm_map(vmm, GVA_A, PA_A, VMM_PAGE_SIZE, VMM_PERM_RW));
  CHECK_OK(vmm_map(vmm, GVA_FAR, PA_A, VMM_PAGE_SIZE, VMM_PERM_R));

  CHECK_OK(vmm_write32(vmm, GVA_A + 0x100, 0x11223344u));
  uint32_t seen = 0;
  CHECK_OK(vmm_read32(vmm, GVA_FAR + 0x100, &seen));
  CHECK(seen == 0x11223344u);

  /* The mirror is read-only even though the original is RW. */
  CHECK_CODE(vmm_write32(vmm, GVA_FAR + 0x100, 0), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_PERMISSION);

  CHECK_OK(vmm_unmap(vmm, GVA_A, VMM_PAGE_SIZE));
  CHECK_OK(vmm_unmap(vmm, GVA_FAR, VMM_PAGE_SIZE));
}

static void test_faults(VMM_Context *vmm)
{
  uint32_t scratch = 0;

  /* Unmapped, in a slab with no L2 at all. */
  CHECK_CODE(vmm_read32(vmm, GVA_FAR, &scratch), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_UNMAPPED);
  CHECK(vmm_last_fault(vmm)->gva == GVA_FAR);
  CHECK(vmm_last_fault(vmm)->required_perms == VMM_PERM_R);

  /* Out of range. */
  CHECK_CODE(vmm_read32(vmm, VMM_ADDRESS_SPACE_SIZE, &scratch), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_OUT_OF_RANGE);
  CHECK_CODE(vmm_write8(vmm, ~(uint64_t)0, 1), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_OUT_OF_RANGE);

  /* Read-only page: reads ok, writes fault PERMISSION. */
  CHECK_OK(vmm_map(vmm, GVA_A, PA_A, VMM_PAGE_SIZE, VMM_PERM_R));
  CHECK_OK(vmm_read32(vmm, GVA_A, &scratch));
  CHECK_CODE(vmm_write32(vmm, GVA_A, 1), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_PERMISSION);
  CHECK(vmm_last_fault(vmm)->required_perms == VMM_PERM_W);
  CHECK(vmm_last_fault(vmm)->gva == GVA_A);

  /* Unmapped page inside a slab that HAS an L2 (PTE == 0 path). */
  CHECK_CODE(vmm_read32(vmm, GVA_A + VMM_PAGE_SIZE, &scratch), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_UNMAPPED);
  CHECK_OK(vmm_unmap(vmm, GVA_A, VMM_PAGE_SIZE));

  /* PERM_NONE: mapped (query says so), every access faults PERMISSION. */
  CHECK_OK(vmm_map(vmm, GVA_A, PA_A, VMM_PAGE_SIZE, VMM_PERM_NONE));
  expect_query(vmm, GVA_A, GVA_A, VMM_PAGE_SIZE, true, VMM_PERM_NONE);
  CHECK_CODE(vmm_read8(vmm, GVA_A, (uint8_t *)&scratch), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_PERMISSION);
  /* ...and it is not a hole: mapping over it is rejected. */
  CHECK_CODE(vmm_map(vmm, GVA_A, PA_B, VMM_PAGE_SIZE, VMM_PERM_RW), RESULT_INVALID_ARGUMENT);
  CHECK_OK(vmm_unmap(vmm, GVA_A, VMM_PAGE_SIZE));
}

static void test_reprotect(VMM_Context *vmm)
{
  CHECK_OK(vmm_map(vmm, GVA_A, PA_A, 2 * VMM_PAGE_SIZE, VMM_PERM_R));
  CHECK_CODE(vmm_write32(vmm, GVA_A, 1), RESULT_MEMORY_FAULT);

  CHECK_OK(vmm_reprotect(vmm, GVA_A, VMM_PAGE_SIZE, VMM_PERM_RW));
  CHECK_OK(vmm_write32(vmm, GVA_A, 1));
  CHECK_CODE(vmm_write32(vmm, GVA_A + VMM_PAGE_SIZE, 1), RESULT_MEMORY_FAULT);
  expect_query(vmm, GVA_A, GVA_A, VMM_PAGE_SIZE, true, VMM_PERM_RW);
  expect_query(vmm, GVA_A + VMM_PAGE_SIZE, GVA_A + VMM_PAGE_SIZE, VMM_PAGE_SIZE, true, VMM_PERM_R);

  /* Range that touches an unmapped page: rejected, nothing changes. */
  CHECK_CODE(vmm_reprotect(vmm, GVA_A, 3 * VMM_PAGE_SIZE, VMM_PERM_ALL), RESULT_INVALID_ARGUMENT);
  expect_query(vmm, GVA_A, GVA_A, VMM_PAGE_SIZE, true, VMM_PERM_RW);
  expect_query(vmm, GVA_A + VMM_PAGE_SIZE, GVA_A + VMM_PAGE_SIZE, VMM_PAGE_SIZE, true, VMM_PERM_R);

  /* Bad perms, unaligned. */
  CHECK_CODE(vmm_reprotect(vmm, GVA_A, VMM_PAGE_SIZE, 0x10), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_reprotect(vmm, GVA_A + 4, VMM_PAGE_SIZE, VMM_PERM_R), RESULT_INVALID_ARGUMENT);

  /* Backing page is unchanged by reprotect. */
  uint32_t v = 0;
  CHECK_OK(vmm_reprotect(vmm, GVA_A, VMM_PAGE_SIZE, VMM_PERM_R));
  CHECK_OK(vmm_read32(vmm, GVA_A, &v));
  CHECK(v == 1);

  CHECK_OK(vmm_unmap(vmm, GVA_A, 2 * VMM_PAGE_SIZE));
}

static void test_unmap(VMM_Context *vmm)
{
  CHECK_OK(vmm_map(vmm, GVA_A, PA_A, 4 * VMM_PAGE_SIZE, VMM_PERM_RW));

  /* Punch a hole in the middle. */
  CHECK_OK(vmm_unmap(vmm, GVA_A + VMM_PAGE_SIZE, 2 * VMM_PAGE_SIZE));
  expect_query(vmm, GVA_A, GVA_A, VMM_PAGE_SIZE, true, VMM_PERM_RW);
  expect_query(vmm, GVA_A + VMM_PAGE_SIZE, GVA_A + VMM_PAGE_SIZE, 2 * VMM_PAGE_SIZE, false, VMM_PERM_NONE);
  expect_query(vmm, GVA_A + 3 * VMM_PAGE_SIZE, GVA_A + 3 * VMM_PAGE_SIZE, VMM_PAGE_SIZE, true, VMM_PERM_RW);

  uint32_t v;
  CHECK_CODE(vmm_read32(vmm, GVA_A + VMM_PAGE_SIZE, &v), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_UNMAPPED);

  /* Unmapping a range with a hole in it: rejected, nothing changes. */
  CHECK_CODE(vmm_unmap(vmm, GVA_A, 4 * VMM_PAGE_SIZE), RESULT_INVALID_ARGUMENT);
  expect_query(vmm, GVA_A, GVA_A, VMM_PAGE_SIZE, true, VMM_PERM_RW);
  CHECK_CODE(vmm_unmap(vmm, GVA_FAR, VMM_PAGE_SIZE), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_unmap(vmm, GVA_A + 1, VMM_PAGE_SIZE), RESULT_INVALID_ARGUMENT);

  CHECK_OK(vmm_unmap(vmm, GVA_A, VMM_PAGE_SIZE));
  CHECK_OK(vmm_unmap(vmm, GVA_A + 3 * VMM_PAGE_SIZE, VMM_PAGE_SIZE));

  /* The slab is now empty: its L2 was released, the L1 slot is zero. */
  CHECK(vmm_page_table_l1(vmm)[(GVA_A >> VMM_PAGE_BITS) >> VMM_L2_INDEX_BITS] == 0);
}

static void test_cross_page_access(VMM_Context *vmm)
{
  const uint64_t straddle = GVA_A + VMM_PAGE_SIZE - 4; /* 4 bytes in page 0, 4 in page 1 */

  CHECK_OK(vmm_map(vmm, GVA_A, PA_A, 2 * VMM_PAGE_SIZE, VMM_PERM_RW));
  CHECK_OK(vmm_write64(vmm, straddle, 0x8877665544332211ull));

  /* Bytes split across the two physical pages, little-endian. */
  CHECK(physical(PA_A)[VMM_PAGE_SIZE - 4] == 0x11);
  CHECK(physical(PA_A)[VMM_PAGE_SIZE - 1] == 0x44);
  CHECK(physical(PA_A + VMM_PAGE_SIZE)[0] == 0x55);
  CHECK(physical(PA_A + VMM_PAGE_SIZE)[3] == 0x88);

  uint64_t v = 0;
  CHECK_OK(vmm_read64(vmm, straddle, &v));
  CHECK(v == 0x8877665544332211ull);

  uint16_t h = 0;
  CHECK_OK(vmm_read16(vmm, GVA_A + VMM_PAGE_SIZE - 1, &h));
  CHECK(h == 0x5544);

  /* Second page goes away: the access must fault and leave page 0 alone. */
  CHECK_OK(vmm_unmap(vmm, GVA_A + VMM_PAGE_SIZE, VMM_PAGE_SIZE));
  CHECK_CODE(vmm_write64(vmm, straddle, 0), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_UNMAPPED);
  CHECK(vmm_last_fault(vmm)->gva == GVA_A + VMM_PAGE_SIZE);
  CHECK(physical(PA_A)[VMM_PAGE_SIZE - 4] == 0x11);
  CHECK(physical(PA_A)[VMM_PAGE_SIZE - 1] == 0x44);
  v = 0x5A5A;
  CHECK_CODE(vmm_read64(vmm, straddle, &v), RESULT_MEMORY_FAULT);
  CHECK(v == 0x5A5A); /* out untouched on fault */

  /* Second page present but read-only: write faults PERMISSION there. */
  CHECK_OK(vmm_map(vmm, GVA_A + VMM_PAGE_SIZE, PA_A + VMM_PAGE_SIZE, VMM_PAGE_SIZE, VMM_PERM_R));
  CHECK_CODE(vmm_write64(vmm, straddle, 0), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_PERMISSION);
  CHECK_OK(vmm_read64(vmm, straddle, &v));
  CHECK(v == 0x8877665544332211ull);

  CHECK_OK(vmm_unmap(vmm, GVA_A, 2 * VMM_PAGE_SIZE));
}

static void test_block_copies(VMM_Context *vmm)
{
  /* Three virtual pages, scattered physical backing: A, B, A+2. */
  CHECK_OK(vmm_map(vmm, GVA_A, PA_A, VMM_PAGE_SIZE, VMM_PERM_RW));
  CHECK_OK(vmm_map(vmm, GVA_A + VMM_PAGE_SIZE, PA_B, VMM_PAGE_SIZE, VMM_PERM_RW));
  CHECK_OK(vmm_map(vmm, GVA_A + 2 * VMM_PAGE_SIZE, PA_A + 2 * VMM_PAGE_SIZE, VMM_PAGE_SIZE, VMM_PERM_RW));

  enum { BLOCK = 9000 };
  static uint8_t src[BLOCK];
  static uint8_t dst[BLOCK];
  for (size_t i = 0; i < BLOCK; i++)
    src[i] = (uint8_t)(i * 7 + 3);

  const uint64_t start = GVA_A + 100; /* unaligned start, spans all 3 pages */
  CHECK_OK(vmm_write_block(vmm, start, src, BLOCK));
  memset(dst, 0, BLOCK);
  CHECK_OK(vmm_read_block(vmm, start, dst, BLOCK));
  CHECK(memcmp(src, dst, BLOCK) == 0);

  /* Middle page really is PA_B. */
  CHECK(physical(PA_B)[0] == src[VMM_PAGE_SIZE - 100]);
  CHECK(physical(PA_B)[VMM_PAGE_SIZE - 1] == src[2 * VMM_PAGE_SIZE - 100 - 1]);

  /* size 0 is a no-op. */
  CHECK_OK(vmm_read_block(vmm, GVA_FAR, dst, 0));
  CHECK_OK(vmm_write_block(vmm, GVA_FAR, src, 0));

  /* Block that runs into a hole: fault, nothing written, out untouched. */
  memset(physical(PA_A + 2 * VMM_PAGE_SIZE), 0x00, (size_t)VMM_PAGE_SIZE);
  CHECK_CODE(vmm_write_block(vmm, GVA_A + 2 * VMM_PAGE_SIZE, src, VMM_PAGE_SIZE + 1), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_UNMAPPED);
  CHECK(vmm_last_fault(vmm)->gva == GVA_A + 3 * VMM_PAGE_SIZE);
  for (uint64_t i = 0; i < VMM_PAGE_SIZE; i++)
    CHECK(physical(PA_A + 2 * VMM_PAGE_SIZE)[i] == 0);
  memset(dst, 0xEE, BLOCK);
  CHECK_CODE(vmm_read_block(vmm, GVA_A + 2 * VMM_PAGE_SIZE, dst, VMM_PAGE_SIZE + 1), RESULT_MEMORY_FAULT);
  CHECK(dst[0] == 0xEE && dst[VMM_PAGE_SIZE] == 0xEE);

  /* Overflowing the address space is a fault, not a wrap. */
  CHECK_CODE(vmm_read_block(vmm, LAST_PAGE_GVA, dst, 2 * VMM_PAGE_SIZE), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_OUT_OF_RANGE);

  CHECK_OK(vmm_unmap(vmm, GVA_A, 3 * VMM_PAGE_SIZE));
}

static void test_guest_to_host(VMM_Context *vmm)
{
  void *host = NULL;

  /* Contiguous physical backing. */
  CHECK_OK(vmm_map(vmm, GVA_A, PA_A, 2 * VMM_PAGE_SIZE, VMM_PERM_RW));
  /* Scattered third page. */
  CHECK_OK(vmm_map(vmm, GVA_A + 2 * VMM_PAGE_SIZE, PA_B, VMM_PAGE_SIZE, VMM_PERM_R));

  vmm_borrow_scope_begin(vmm);

  CHECK_OK(vmm_guest_to_host(vmm, GVA_A + 0x80, 2 * VMM_PAGE_SIZE - 0x80, VMM_PERM_RW, &host));
  CHECK(host == physical(PA_A + 0x80));
  ((uint8_t *)host)[0] = 0x42;
  uint8_t seen = 0;
  CHECK_OK(vmm_read8(vmm, GVA_A + 0x80, &seen));
  CHECK(seen == 0x42);

  /* Spanning into the scattered page: not contiguous. */
  CHECK_CODE(vmm_guest_to_host(vmm, GVA_A + VMM_PAGE_SIZE, 2 * VMM_PAGE_SIZE, VMM_PERM_R, &host),
             RESULT_NOT_CONTIGUOUS);

  /* Missing permission anywhere in the range: memory fault. */
  CHECK_CODE(vmm_guest_to_host(vmm, GVA_A + 2 * VMM_PAGE_SIZE, 16, VMM_PERM_W, &host), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_PERMISSION);
  CHECK_CODE(vmm_guest_to_host(vmm, GVA_A, 3 * VMM_PAGE_SIZE, VMM_PERM_W, &host), RESULT_MEMORY_FAULT);

  /* Running past the end of the mapping. */
  CHECK_CODE(vmm_guest_to_host(vmm, GVA_A + 2 * VMM_PAGE_SIZE, VMM_PAGE_SIZE + 1, VMM_PERM_R, &host),
             RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_UNMAPPED);

  /* Argument validation. */
  CHECK_CODE(vmm_guest_to_host(vmm, GVA_A, 0, VMM_PERM_R, &host), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_guest_to_host(vmm, GVA_A, 16, VMM_PERM_NONE, &host), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_guest_to_host(vmm, GVA_A, 16, VMM_PERM_R, NULL), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_guest_to_host(vmm, LAST_PAGE_GVA, 2 * VMM_PAGE_SIZE, VMM_PERM_R, &host), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_OUT_OF_RANGE);

  vmm_borrow_scope_end(vmm);

  CHECK_OK(vmm_unmap(vmm, GVA_A, 3 * VMM_PAGE_SIZE));
}

static void test_query(VMM_Context *vmm)
{
  VMM_Region_Info info;
  CHECK_CODE(vmm_query(NULL, GVA_A, &info), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_query(vmm, GVA_A, NULL), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(vmm_query(vmm, VMM_ADDRESS_SPACE_SIZE, &info), RESULT_INVALID_ARGUMENT);

  /* Empty address space: one hole. */
  expect_query(vmm, 0, 0, VMM_ADDRESS_SPACE_SIZE, false, VMM_PERM_NONE);
  expect_query(vmm, GVA_FAR + 123, 0, VMM_ADDRESS_SPACE_SIZE, false, VMM_PERM_NONE);
  expect_query(vmm, VMM_ADDRESS_SPACE_SIZE - 1, 0, VMM_ADDRESS_SPACE_SIZE, false, VMM_PERM_NONE);

  /* A mapping that spans an L2 boundary (starts 2 pages before slab 3)
   * must be reported as ONE run. Physical backing is irrelevant. */
  const uint64_t slab3 = 3 * VMM_L2_SPAN_BYTES;
  const uint64_t base = slab3 - 2 * VMM_PAGE_SIZE;
  CHECK_OK(vmm_map(vmm, base, PA_A, 2 * VMM_PAGE_SIZE, VMM_PERM_RX));
  CHECK_OK(vmm_map(vmm, slab3, PA_B, 3 * VMM_PAGE_SIZE, VMM_PERM_RX));
  expect_query(vmm, slab3 + 5, base, 5 * VMM_PAGE_SIZE, true, VMM_PERM_RX);
  expect_query(vmm, base, base, 5 * VMM_PAGE_SIZE, true, VMM_PERM_RX);

  /* Holes on either side are reported with exact extents. */
  expect_query(vmm, 0, 0, base, false, VMM_PERM_NONE);
  expect_query(vmm, slab3 + 3 * VMM_PAGE_SIZE, slab3 + 3 * VMM_PAGE_SIZE,
               VMM_ADDRESS_SPACE_SIZE - (slab3 + 3 * VMM_PAGE_SIZE), false, VMM_PERM_NONE);

  /* A permission change splits the run. */
  CHECK_OK(vmm_reprotect(vmm, slab3 + VMM_PAGE_SIZE, VMM_PAGE_SIZE, VMM_PERM_R));
  expect_query(vmm, base, base, 3 * VMM_PAGE_SIZE, true, VMM_PERM_RX);
  expect_query(vmm, slab3 + VMM_PAGE_SIZE, slab3 + VMM_PAGE_SIZE, VMM_PAGE_SIZE, true, VMM_PERM_R);
  expect_query(vmm, slab3 + 2 * VMM_PAGE_SIZE, slab3 + 2 * VMM_PAGE_SIZE, VMM_PAGE_SIZE, true, VMM_PERM_RX);

  /* Last page of the address space. */
  CHECK_OK(vmm_map(vmm, LAST_PAGE_GVA, PA_A, VMM_PAGE_SIZE, VMM_PERM_R));
  expect_query(vmm, LAST_PAGE_GVA + 17, LAST_PAGE_GVA, VMM_PAGE_SIZE, true, VMM_PERM_R);
  expect_query(vmm, slab3 + 3 * VMM_PAGE_SIZE, slab3 + 3 * VMM_PAGE_SIZE,
               LAST_PAGE_GVA - (slab3 + 3 * VMM_PAGE_SIZE), false, VMM_PERM_NONE);

  CHECK_OK(vmm_unmap(vmm, LAST_PAGE_GVA, VMM_PAGE_SIZE));
  CHECK_OK(vmm_unmap(vmm, base, 5 * VMM_PAGE_SIZE));
  expect_query(vmm, 0, 0, VMM_ADDRESS_SPACE_SIZE, false, VMM_PERM_NONE);
}

static void test_l2_exhaustion_and_recycling(VMM_Context *vmm)
{
  /* One page in each of VMM_L2_TABLE_CAPACITY distinct slabs. */
  for (uint64_t i = 0; i < VMM_L2_TABLE_CAPACITY; i++)
    CHECK_OK(vmm_map(vmm, i * VMM_L2_SPAN_BYTES, PA_A, VMM_PAGE_SIZE, VMM_PERM_R));

  /* Slab #capacity has no table left. */
  const uint64_t extra = VMM_L2_TABLE_CAPACITY * VMM_L2_SPAN_BYTES;
  CHECK_CODE(vmm_map(vmm, extra, PA_A, VMM_PAGE_SIZE, VMM_PERM_R), RESULT_OUT_OF_MEMORY);
  /* Still a hole - one that starts right after the last slab's single
   * mapped page and runs to the end of the address space. */
  const uint64_t hole_start = (VMM_L2_TABLE_CAPACITY - 1) * VMM_L2_SPAN_BYTES + VMM_PAGE_SIZE;
  expect_query(vmm, extra, hole_start, VMM_ADDRESS_SPACE_SIZE - hole_start, false, VMM_PERM_NONE);

  /* Growing an existing slab needs no new table. */
  CHECK_OK(vmm_map(vmm, VMM_PAGE_SIZE, PA_B, VMM_PAGE_SIZE, VMM_PERM_R));
  CHECK_OK(vmm_unmap(vmm, VMM_PAGE_SIZE, VMM_PAGE_SIZE));

  /* Free one slab, then ask for TWO new slabs in one call: refused as a
   * whole, and neither slab is created (validate-then-commit). */
  CHECK_OK(vmm_unmap(vmm, 7 * VMM_L2_SPAN_BYTES, VMM_PAGE_SIZE));
  const uint64_t two_slabs = extra + VMM_L2_SPAN_BYTES - VMM_PAGE_SIZE;
  CHECK_CODE(vmm_map(vmm, two_slabs, PA_A, 2 * VMM_PAGE_SIZE, VMM_PERM_R), RESULT_OUT_OF_MEMORY);
  const uint64_t *l1 = vmm_page_table_l1(vmm);
  CHECK(l1[VMM_L2_TABLE_CAPACITY] == 0);
  CHECK(l1[VMM_L2_TABLE_CAPACITY + 1] == 0);

  /* The recycled table serves a brand-new slab. */
  CHECK_OK(vmm_map(vmm, extra, PA_A, VMM_PAGE_SIZE, VMM_PERM_R));
  uint8_t b = 0;
  CHECK_OK(vmm_read8(vmm, extra, &b));

  /* Tear it all down; the address space is empty again. */
  CHECK_OK(vmm_unmap(vmm, extra, VMM_PAGE_SIZE));
  for (uint64_t i = 0; i < VMM_L2_TABLE_CAPACITY; i++)
  {
    if (i == 7) continue;
    CHECK_OK(vmm_unmap(vmm, i * VMM_L2_SPAN_BYTES, VMM_PAGE_SIZE));
  }
  for (uint64_t i = 0; i < VMM_L1_ENTRY_COUNT; i++)
    CHECK(l1[i] == 0);
  expect_query(vmm, 0, 0, VMM_ADDRESS_SPACE_SIZE, false, VMM_PERM_NONE);

  /* And the full capacity is available again (all tables recycled). */
  for (uint64_t i = 0; i < VMM_L2_TABLE_CAPACITY; i++)
    CHECK_OK(vmm_map(vmm, i * VMM_L2_SPAN_BYTES, PA_A, VMM_PAGE_SIZE, VMM_PERM_R));
  for (uint64_t i = 0; i < VMM_L2_TABLE_CAPACITY; i++)
    CHECK_OK(vmm_unmap(vmm, i * VMM_L2_SPAN_BYTES, VMM_PAGE_SIZE));
}

static void test_inline_matches_checked(VMM_Context *vmm)
{
  const uint64_t *l1 = vmm_page_table_l1(vmm);
  VMM_Fault fault;
  uint32_t a = 0, b = 0;
  uint64_t wide_a = 0, wide_b = 0;

  CHECK_OK(vmm_map(vmm, GVA_A, PA_A, VMM_PAGE_SIZE, VMM_PERM_RW));
  CHECK_OK(vmm_map(vmm, GVA_A + VMM_PAGE_SIZE, PA_B, VMM_PAGE_SIZE, VMM_PERM_R));

  /* Same value through both paths. */
  CHECK(vmm_write32_inline(l1, GVA_A + 8, 0xFACEFEEDu, &fault));
  CHECK_OK(vmm_read32(vmm, GVA_A + 8, &a));
  CHECK(vmm_read32_inline(l1, GVA_A + 8, &b, &fault));
  CHECK(a == 0xFACEFEEDu && b == 0xFACEFEEDu);

  /* Cross-page through both paths. */
  CHECK_CODE(vmm_write64(vmm, GVA_A + VMM_PAGE_SIZE - 3, 0x1122334455667788ull), RESULT_MEMORY_FAULT);
  CHECK(!vmm_write64_inline(l1, GVA_A + VMM_PAGE_SIZE - 3, 0, &fault));
  CHECK(fault.kind == VMM_FAULT_PERMISSION && fault.gva == GVA_A + VMM_PAGE_SIZE);
  CHECK_OK(vmm_reprotect(vmm, GVA_A + VMM_PAGE_SIZE, VMM_PAGE_SIZE, VMM_PERM_RW));
  CHECK_OK(vmm_write64(vmm, GVA_A + VMM_PAGE_SIZE - 3, 0x1122334455667788ull));
  CHECK_OK(vmm_read64(vmm, GVA_A + VMM_PAGE_SIZE - 3, &wide_a));
  CHECK(vmm_read64_inline(l1, GVA_A + VMM_PAGE_SIZE - 3, &wide_b, &fault));
  CHECK(wide_a == 0x1122334455667788ull && wide_b == wide_a);

  /* Same fault kinds. */
  CHECK(!vmm_read32_inline(l1, GVA_FAR, &b, &fault));
  CHECK(fault.kind == VMM_FAULT_UNMAPPED && fault.gva == GVA_FAR && fault.required_perms == VMM_PERM_R);
  CHECK_CODE(vmm_read32(vmm, GVA_FAR, &a), RESULT_MEMORY_FAULT);
  CHECK(vmm_last_fault(vmm)->kind == VMM_FAULT_UNMAPPED);

  CHECK(!vmm_read8_inline(l1, VMM_ADDRESS_SPACE_SIZE + 5, (uint8_t *)&b, &fault));
  CHECK(fault.kind == VMM_FAULT_OUT_OF_RANGE);

  CHECK(vmm_translate_inline(l1, GVA_A + 0x123, VMM_PERM_R, &fault) == physical(PA_A + 0x123));
  CHECK(vmm_translate_inline(l1, GVA_A, VMM_PERM_X, &fault) == NULL);
  CHECK(fault.kind == VMM_FAULT_PERMISSION && fault.required_perms == VMM_PERM_X);

  CHECK_OK(vmm_unmap(vmm, GVA_A, 2 * VMM_PAGE_SIZE));
}

static void test_borrow_scopes(VMM_Context *vmm)
{
  /* Borrow inside a scope, end the scope, then remap the same range: the
   * borrow was released at scope end, so no (debug) assert fires. */
  void *host = NULL;
  CHECK_OK(vmm_map(vmm, GVA_A, PA_A, VMM_PAGE_SIZE, VMM_PERM_RW));
  vmm_borrow_scope_begin(vmm);
  CHECK_OK(vmm_guest_to_host(vmm, GVA_A, VMM_PAGE_SIZE, VMM_PERM_R, &host));
  /* Mutating a DIFFERENT range while a borrow is live is fine. */
  CHECK_OK(vmm_map(vmm, GVA_FAR, PA_B, VMM_PAGE_SIZE, VMM_PERM_R));
  CHECK_OK(vmm_unmap(vmm, GVA_FAR, VMM_PAGE_SIZE));
  vmm_borrow_scope_end(vmm);
  CHECK_OK(vmm_unmap(vmm, GVA_A, VMM_PAGE_SIZE));
  CHECK_OK(vmm_map(vmm, GVA_A, PA_B, VMM_PAGE_SIZE, VMM_PERM_RW));
  CHECK_OK(vmm_unmap(vmm, GVA_A, VMM_PAGE_SIZE));

  /* Scopes are reusable. */
  vmm_borrow_scope_begin(vmm);
  vmm_borrow_scope_end(vmm);
  vmm_borrow_scope_begin(vmm);
  vmm_borrow_scope_end(vmm);
}

int main(void)
{
  test_lifecycle();

  CHECK_OK(layout_create());
  VMM_Context *vmm = vmm_create();
  CHECK(vmm != NULL);

  test_map_validation(vmm);
  test_round_trip_and_physical_backing(vmm);
  test_mirrors(vmm);
  test_faults(vmm);
  test_reprotect(vmm);
  test_unmap(vmm);
  test_cross_page_access(vmm);
  test_block_copies(vmm);
  test_guest_to_host(vmm);
  test_query(vmm);
  test_l2_exhaustion_and_recycling(vmm);
  test_inline_matches_checked(vmm);
  test_borrow_scopes(vmm);

  /* Every test leaves the address space empty. */
  for (uint64_t i = 0; i < VMM_L1_ENTRY_COUNT; i++)
    CHECK(vmm_page_table_l1(vmm)[i] == 0);

  vmm_destroy(vmm);
  layout_destroy();

  printf("[vmm_test] OK\n");
  return 0;
}
