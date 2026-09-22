/**
 * core/hle/kernel/svc_memory unit test: SetHeapSize, MapMemory,
 * UnmapMemory, QueryMemory driven the way thread_test.c drives thread
 * SVCs - registers set by hand, hle_on_svc invoked directly, state
 * asserted through the CPU backend and vmm (§5), never through IPC (does
 * not exist yet).
 */
#define CHECK_NAME "svc_memory_test"
#include "check.h"

#include "common/arena.h"
#include "emulator.h"
#include "hle/hle.h"
#include "hle/kernel/svc_memory.h"
#include "loader_fixtures.h"

#include <string.h>

#define PAGE VMM_PAGE_SIZE
#define GRANULE PROCESS_HEAP_SIZE_GRANULE
#define STACK_BYTES 0x80000u
#define PROGRAM_ID 0x0100000000043000ull

static const uint32_t k_caps[] = {0x7u | (44u << 4) | (28u << 10) | (0u << 16) | (2u << 24),
                                  0xFu | (0xFFu << 5)};

/* One tiny "main" module (.text only) is enough process to exercise the
 * memory SVCs against a real address_space/vmm/pages - this test is not
 * re-testing the loader (process_test.c already does that thoroughly). */
static void bootstrap_minimal(Emulator *emu) {
  static uint8_t text[PAGE];
  memset(text, 0xC3, sizeof(text));
  Fixture_NSO_Params nso_params;
  memset(&nso_params, 0, sizeof(nso_params));
  nso_params.text = (Fixture_NSO_Segment){text, sizeof(text), 0, false, false};
  /* rodata/data are zero-size but still need memory_offset placed after
   * .text (page-aligned) - nso_open rejects segments that are out of
   * order even when a later one is empty. A valid (unused) buffer
   * pointer is passed regardless of size, matching process_test.c's
   * "sdk" spec (also a real zero-size rodata). */
  static const uint8_t empty_segment[1] = {0};
  nso_params.rodata = (Fixture_NSO_Segment){empty_segment, 0, (uint32_t)sizeof(text), false, false};
  nso_params.data = (Fixture_NSO_Segment){empty_segment, 0, (uint32_t)sizeof(text), false, false};
  uint8_t module_id[0x20];
  memset(module_id, 0xAB, sizeof(module_id));
  nso_params.module_id = module_id;
  Fixture_Buffer nso_image;
  fixture_buffer_init(&nso_image);
  fixture_build_nso(&nso_params, &nso_image);

  Fixture_NPDM_Params npdm_params;
  memset(&npdm_params, 0, sizeof(npdm_params));
  npdm_params.flags = (uint8_t)(0x01 | (3u << 1)); /* 39-bit address space */
  npdm_params.main_thread_priority = 44;
  npdm_params.main_thread_stack_size = STACK_BYTES;
  npdm_params.name = "SvcMemTest";
  npdm_params.program_id_min = PROGRAM_ID;
  npdm_params.program_id_max = PROGRAM_ID;
  npdm_params.acid_capabilities = k_caps;
  npdm_params.acid_capability_count = 2;
  npdm_params.program_id = PROGRAM_ID;
  npdm_params.aci0_capabilities = k_caps;
  npdm_params.aci0_capability_count = 2;
  Fixture_Buffer npdm_image;
  fixture_buffer_init(&npdm_image);
  fixture_build_npdm(&npdm_params, &npdm_image);

  Fixture_File files[2] = {
      {EXEFS_FILE_NPDM, npdm_image.bytes, npdm_image.size},
      {EXEFS_FILE_MAIN, nso_image.bytes, nso_image.size},
  };
  Fixture_Buffer exefs_image;
  fixture_buffer_init(&exefs_image);
  fixture_build_pfs0(files, 2, &exefs_image);

  CHECK_OK(emulator_create(emu));

  Arena scratch;
  CHECK(arena_create(&scratch, 256 * 1024));
  Arena exefs_arena;
  CHECK(arena_create(&exefs_arena, 256 * 1024));

  Byte_Source exefs_source = byte_source_from_memory(exefs_image.bytes, exefs_image.size);
  ExeFS exefs;
  CHECK_OK(exefs_open(&exefs_source, &exefs_arena, &exefs));
  NPDM npdm;
  CHECK_OK(npdm_parse(npdm_image.bytes, npdm_image.size, &npdm));

  const Process_Bootstrap_Params params = {
      .exefs = &exefs, .npdm = &npdm, .vmm = emu->vmm, .pages = &emu->pages,
      .scratch = &scratch, .aslr_seed = 0,
  };
  /* Bootstraps directly into emu->process - the same field emu->hle.process
   * already points at (emulator_create wired that up before this process
   * existed; see hle.h's doc comment on the pointer-before-populated
   * pattern). */
  CHECK_OK(process_bootstrap(&params, &emu->process));

  arena_destroy(&exefs_arena);
  arena_destroy(&scratch);
  fixture_buffer_free(&nso_image);
  fixture_buffer_free(&npdm_image);
  fixture_buffer_free(&exefs_image);
}

static void set_svc_args3(CPU_Register_File *regs, uint64_t x0, uint64_t x1, uint64_t x2) {
  regs->x[0] = x0;
  regs->x[1] = x1;
  regs->x[2] = x2;
}

int main(void) {
  Emulator emu;
  bootstrap_minimal(&emu);
  const Process *process = &emu.process;
  CPU_Register_File *regs = emu.cpu_backend->get_register_file(emu.cpu_state);
  const Address_Region heap = process->address_space.heap;
  uint8_t byte = 0;

  /* --- SetHeapSize --- */

  /* Not a PROCESS_HEAP_SIZE_GRANULE multiple. */
  set_svc_args3(regs, 0, PAGE, 0);
  hle_on_svc(emu.cpu_state, 0x01, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_INVALID_SIZE);
  CHECK(emu.process.heap_size == 0);

  /* Bigger than the fixed heap region. */
  set_svc_args3(regs, 0, heap.size + GRANULE, 0);
  hle_on_svc(emu.cpu_state, 0x01, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_INVALID_MEMORY_RANGE);

  /* Grow to one granule. Heap base is fixed by address_space_init at
   * bootstrap, not re-randomized per call. */
  set_svc_args3(regs, 0, GRANULE, 0);
  hle_on_svc(emu.cpu_state, 0x01, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
  CHECK(regs->x[1] == heap.base);
  CHECK(emu.process.heap_size == GRANULE);
  CHECK_OK(vmm_write8(emu.vmm, heap.base, 0x42));

  /* Grow again: the first granule's data survives, the new half is
   * really RW. */
  set_svc_args3(regs, 0, 2 * GRANULE, 0);
  hle_on_svc(emu.cpu_state, 0x01, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
  CHECK(emu.process.heap_size == 2 * GRANULE);
  CHECK_OK(vmm_read8(emu.vmm, heap.base, &byte));
  CHECK(byte == 0x42);
  CHECK_OK(vmm_write8(emu.vmm, heap.base + GRANULE, 0x99));

  /* Shrink back to one granule: the second half is released and
   * unmapped, the first is untouched. */
  set_svc_args3(regs, 0, GRANULE, 0);
  hle_on_svc(emu.cpu_state, 0x01, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
  CHECK(emu.process.heap_size == GRANULE);
  CHECK_OK(vmm_read8(emu.vmm, heap.base, &byte));
  CHECK(byte == 0x42);
  VMM_Region_Info shrunk_info;
  CHECK_OK(vmm_query(emu.vmm, heap.base + GRANULE, &shrunk_info));
  CHECK(!shrunk_info.is_mapped);

  /* --- MapMemory / UnmapMemory --- */

  /* dst sits well clear of the real main-thread stack the bootstrap
   * already mapped. */
  const uint64_t dst = process->main_thread_stack.base + process->main_thread_stack.size + PAGE;
  const uint64_t src = heap.base; /* still committed, still holds 0x42 */

  /* Rejections that must not change any state. */
  set_svc_args3(regs, dst, src, 0); /* zero size */
  hle_on_svc(emu.cpu_state, 0x04, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_INVALID_SIZE);

  set_svc_args3(regs, dst + 1, src, PAGE); /* dst misaligned */
  hle_on_svc(emu.cpu_state, 0x04, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_INVALID_SIZE);

  set_svc_args3(regs, 0x1000, src, PAGE); /* dst not in the stack region */
  hle_on_svc(emu.cpu_state, 0x04, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_INVALID_MEMORY_RANGE);

  set_svc_args3(regs, dst, process->address_space.code.base, PAGE); /* src not in the heap region */
  hle_on_svc(emu.cpu_state, 0x04, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_INVALID_MEMORY_RANGE);

  set_svc_args3(regs, process->main_thread_stack.base, src, PAGE); /* dst already mapped */
  hle_on_svc(emu.cpu_state, 0x04, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_INVALID_MEMORY_STATE);

  set_svc_args3(regs, dst, heap.base + heap.size - PAGE, PAGE); /* src past what's committed */
  hle_on_svc(emu.cpu_state, 0x04, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_INVALID_MEMORY_STATE);

  CHECK(emu.process.heap_borrow_count == 0);

  /* Happy path: mirror one heap page into the stack region. */
  set_svc_args3(regs, dst, src, PAGE);
  hle_on_svc(emu.cpu_state, 0x04, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
  CHECK(emu.process.heap_borrow_count == 1);

  /* SetHeapSize cannot shrink across a live borrow's source. */
  set_svc_args3(regs, 0, 0, 0);
  hle_on_svc(emu.cpu_state, 0x01, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_INVALID_MEMORY_STATE);
  CHECK(emu.process.heap_size == GRANULE);

  /* src is now inaccessible - the SAME physical page reads through dst,
   * proving this is a mirror, not a copy. */
  CHECK_CODE(vmm_read8(emu.vmm, src, &byte), RESULT_MEMORY_FAULT);
  CHECK_OK(vmm_read8(emu.vmm, dst, &byte));
  CHECK(byte == 0x42);

  /* QueryMemory sees the borrow on both halves (§3's noted, deliberately
   * partial MemoryType/attr fidelity: these two values ARE modeled
   * precisely because the borrow table is the source of truth for them,
   * unlike the ~25 Horizon types Voland cannot construct yet). */
  {
    const uint64_t out_gva = process->main_thread_tls_gva; /* scratch: mapped, writable */
    HLE_Memory_Info info;

    set_svc_args3(regs, out_gva, 0, src);
    hle_on_svc(emu.cpu_state, 0x06, &emu.hle);
    CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
    CHECK_OK(vmm_read_block(emu.vmm, out_gva, &info, sizeof(info)));
    CHECK(info.type == HLE_MEMTYPE_WEIRD_MAPPED_MEM);
    CHECK(info.perm == VMM_PERM_NONE);
    CHECK(info.attr == 0);

    set_svc_args3(regs, out_gva, 0, dst);
    hle_on_svc(emu.cpu_state, 0x06, &emu.hle);
    CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
    CHECK_OK(vmm_read_block(emu.vmm, out_gva, &info, sizeof(info)));
    CHECK(info.type == HLE_MEMTYPE_MAPPED_MEMORY);
    CHECK(info.perm == VMM_PERM_RW);
  }

  /* A mismatched triple is rejected and changes nothing. */
  set_svc_args3(regs, dst, src, 2 * PAGE);
  hle_on_svc(emu.cpu_state, 0x05, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_INVALID_MEMORY_STATE);
  CHECK(emu.process.heap_borrow_count == 1);

  /* The exact triple reverses it: dst gone, src back to RW with its
   * original content untouched by the reprotect. */
  set_svc_args3(regs, dst, src, PAGE);
  hle_on_svc(emu.cpu_state, 0x05, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
  CHECK(emu.process.heap_borrow_count == 0);
  CHECK_OK(vmm_read8(emu.vmm, src, &byte));
  CHECK(byte == 0x42);
  VMM_Region_Info dst_after;
  CHECK_OK(vmm_query(emu.vmm, dst, &dst_after));
  CHECK(!dst_after.is_mapped);

  /* Borrow table exhaustion, then undo everything so the SetHeapSize
   * shrink-vs-borrow check above cannot be reproduced by an accidental
   * survivor. */
  for (uint32_t i = 0; i < PROCESS_MAX_HEAP_BORROWS; i++) {
    set_svc_args3(regs, dst + (uint64_t)i * PAGE, src + (uint64_t)i * PAGE, PAGE);
    hle_on_svc(emu.cpu_state, 0x04, &emu.hle);
    CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
  }
  CHECK(emu.process.heap_borrow_count == PROCESS_MAX_HEAP_BORROWS);
  set_svc_args3(regs, dst + (uint64_t)PROCESS_MAX_HEAP_BORROWS * PAGE,
               src + (uint64_t)PROCESS_MAX_HEAP_BORROWS * PAGE, PAGE);
  hle_on_svc(emu.cpu_state, 0x04, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_OUT_OF_MEMORY);
  CHECK(emu.process.heap_borrow_count == PROCESS_MAX_HEAP_BORROWS);
  for (uint32_t i = 0; i < PROCESS_MAX_HEAP_BORROWS; i++) {
    set_svc_args3(regs, dst + (uint64_t)i * PAGE, src + (uint64_t)i * PAGE, PAGE);
    hle_on_svc(emu.cpu_state, 0x05, &emu.hle);
    CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
  }
  CHECK(emu.process.heap_borrow_count == 0);

  /* --- QueryMemory across the remaining regions --- */
  {
    const uint64_t out_gva = process->main_thread_tls_gva;
    const struct {
      uint64_t addr;
      uint32_t want_type;
      bool want_mapped;
    } cases[] = {
        {process->entry_point, HLE_MEMTYPE_CODE_STATIC, true},
        {process->main_thread_stack.base, HLE_MEMTYPE_NORMAL, true},
        {process->main_thread_tls_gva, HLE_MEMTYPE_THREAD_LOCAL, true},
        {process->address_space.alias.base, HLE_MEMTYPE_UNMAPPED, false},
        {0x1000, HLE_MEMTYPE_UNMAPPED, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      set_svc_args3(regs, out_gva, 0, cases[i].addr);
      hle_on_svc(emu.cpu_state, 0x06, &emu.hle);
      CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
      CHECK(regs->x[1] == 0); /* pageinfo: always reserved-0 */
      HLE_Memory_Info info;
      CHECK_OK(vmm_read_block(emu.vmm, out_gva, &info, sizeof(info)));
      CHECK(info.type == cases[i].want_type);
      CHECK(info.attr == 0);
      CHECK(info.ipc_refcount == 0 && info.device_refcount == 0);
      CHECK((info.perm != VMM_PERM_NONE) == cases[i].want_mapped);
    }
  }

  /* Unknown query address entirely outside the address space. */
  set_svc_args3(regs, process->main_thread_tls_gva, 0, VMM_ADDRESS_SPACE_SIZE);
  hle_on_svc(emu.cpu_state, 0x06, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_INVALID_MEMORY_RANGE);

  /* --- page_allocator's freelist actually gets used, not just cursor
   * retraction (page_allocator.h: a shrink only hits cursor retraction
   * when the freed range is the bump allocator's most recent extent).
   * Simulates a later, unrelated allocation - what Phase 2's
   * CreateThread will eventually be - so the next shrink's freed range
   * is no longer at the cursor and must go through the freelist. --- */
  {
    set_svc_args3(regs, 0, 2 * GRANULE, 0);
    hle_on_svc(emu.cpu_state, 0x01, &emu.hle);
    CHECK(regs->x[0] == HLE_RESULT_SUCCESS);

    uint64_t unrelated_pa = 0;
    CHECK_OK(page_allocator_allocate(&emu.pages, 1, &unrelated_pa));
    CHECK(emu.pages.free_run_count == 0);

    set_svc_args3(regs, 0, GRANULE, 0);
    hle_on_svc(emu.cpu_state, 0x01, &emu.hle);
    CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
    CHECK(emu.process.heap_size == GRANULE);
    /* The freed second granule is buried below `unrelated_pa` now - it
     * cannot have retracted the cursor, so it must be in the freelist. */
    CHECK(emu.pages.free_run_count == 1);
    CHECK(emu.pages.free_runs[0].page_count == GRANULE / PAGE);

    const uint64_t used_before_regrow = emu.pages.used_bytes;
    set_svc_args3(regs, 0, 2 * GRANULE, 0);
    hle_on_svc(emu.cpu_state, 0x01, &emu.hle);
    CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
    /* Reused the freelist run rather than bumping the cursor. */
    CHECK(emu.pages.used_bytes == used_before_regrow);
    CHECK(emu.pages.free_run_count == 0);

    CHECK_OK(page_allocator_free(&emu.pages, unrelated_pa, 1));
  }

  /* Bootstrapped directly (bypassing emulator_load_program), so
   * emulator_destroy's own teardown path never fires - tear the process
   * down explicitly first, same as process_test.c. */
  process_teardown(&emu.process, emu.vmm, &emu.pages);
  emulator_destroy(&emu);
  printf("[" CHECK_NAME "] ok\n");
  return 0;
}
