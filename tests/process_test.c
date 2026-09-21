/**
 * core/hle/kernel/process unit test: §12 steps 2-4 over a synthesized
 * ExeFS (main.npdm + four NSOs), asserting the guest-visible outcome
 * through vmm only - mappings, permissions, bytes, and the main thread's
 * register file.
 */
#define CHECK_NAME "process_test"
#include "check.h"

#include "common/arena.h"
#include "emulator.h"
#include "hle/kernel/process.h"
#include "loader_fixtures.h"

#include <string.h>

#define SCRATCH_BYTES (512 * 1024)
#define PAGE VMM_PAGE_SIZE
#define STACK_BYTES 0x80000u
#define PROGRAM_ID 0x0100000000042000ull

/* Thread info (prio 44..28, cores 0..2) + SVC mask word, as loader_smoke. */
static const uint32_t k_caps[] = {0x7u | (44u << 4) | (28u << 10) | (0u << 16) | (2u << 24),
                                  0xFu | (0xFFu << 5)};

/* One module's plaintext segments. Sizes chosen so text/rodata/data each
 * end mid-page, bss spills into a new page, and nothing is symmetric. */
typedef struct Module_Spec {
  const char *name;
  uint8_t text_fill;
  size_t text_size;
  size_t rodata_size;
  size_t data_size;
  uint32_t bss_size;
  uint64_t expected_image_size;
} Module_Spec;

static const Module_Spec k_specs[] = {
    {"rtld", 0x11, 0x1000 + 40, 0x100, 0x50, 0x800, 0x4000},
    {"main", 0x22, 0x2345, 0x400, 0x1001, 0x3000, 0x9000},
    {"subsdk1", 0x33, 0x10, 0x10, 0x10, 0, 0x3000},
    {"sdk", 0x44, 0x800, 0x0, 0x20, 0x100, 0x2000},
};
#define SPEC_COUNT (sizeof(k_specs) / sizeof(k_specs[0]))

#define MAX_SEGMENT 0x2400u
static uint8_t g_text[SPEC_COUNT][MAX_SEGMENT];
static uint8_t g_rodata[SPEC_COUNT][MAX_SEGMENT];
static uint8_t g_data[SPEC_COUNT][MAX_SEGMENT];
static Fixture_Buffer g_nso_images[SPEC_COUNT];
static Fixture_Buffer g_npdm_image;

static void build_module_images(void) {
  for (size_t m = 0; m < SPEC_COUNT; m++) {
    const Module_Spec *spec = &k_specs[m];
    for (size_t i = 0; i < spec->text_size; i++) g_text[m][i] = (uint8_t)(spec->text_fill + i % 7);
    for (size_t i = 0; i < spec->rodata_size; i++) g_rodata[m][i] = (uint8_t)(0x80 + spec->text_fill + i);
    for (size_t i = 0; i < spec->data_size; i++) g_data[m][i] = (uint8_t)(0xC0 ^ spec->text_fill ^ (uint8_t)i);

    Fixture_NSO_Params p;
    memset(&p, 0, sizeof(p));
    p.text = (Fixture_NSO_Segment){g_text[m], spec->text_size, 0, true, true};
    const uint32_t rodata_offset = (uint32_t)((spec->text_size + PAGE - 1) & ~(PAGE - 1));
    p.rodata = (Fixture_NSO_Segment){g_rodata[m], spec->rodata_size, rodata_offset, m % 2 == 0, false};
    const uint32_t data_offset = (uint32_t)((rodata_offset + spec->rodata_size + PAGE - 1) & ~(PAGE - 1));
    p.data = (Fixture_NSO_Segment){g_data[m], spec->data_size, data_offset, m % 2 == 1, false};
    p.bss_size = spec->bss_size;
    uint8_t module_id[0x20];
    memset(module_id, spec->text_fill, sizeof(module_id));
    p.module_id = module_id;
    fixture_build_nso(&p, &g_nso_images[m]);
  }
}

static void build_npdm(uint8_t address_space) {
  Fixture_NPDM_Params p;
  memset(&p, 0, sizeof(p));
  p.flags = (uint8_t)(0x01 | (address_space << 1));
  p.main_thread_priority = 44;
  p.main_thread_stack_size = STACK_BYTES;
  p.name = "ProcTest";
  p.program_id_min = PROGRAM_ID;
  p.program_id_max = PROGRAM_ID;
  p.acid_capabilities = k_caps;
  p.acid_capability_count = 2;
  p.program_id = PROGRAM_ID;
  p.aci0_capabilities = k_caps;
  p.aci0_capability_count = 2;
  fixture_buffer_free(&g_npdm_image);
  fixture_build_npdm(&p, &g_npdm_image);
}

/* Builds a PFS0 over main.npdm plus the first `module_count` specs. */
static void build_exefs(size_t module_count, Fixture_Buffer *out) {
  Fixture_File files[1 + SPEC_COUNT];
  files[0] = (Fixture_File){EXEFS_FILE_NPDM, g_npdm_image.bytes, g_npdm_image.size};
  for (size_t m = 0; m < module_count; m++) {
    files[1 + m] = (Fixture_File){k_specs[m].name, g_nso_images[m].bytes, g_nso_images[m].size};
  }
  fixture_build_pfs0(files, (uint32_t)(1 + module_count), out);
}

static void expect_run(VMM_Context *vmm, uint64_t gva, uint64_t size, uint32_t perms) {
  VMM_Region_Info info;
  CHECK_OK(vmm_query(vmm, gva, &info));
  CHECK(info.is_mapped);
  CHECK(info.perms == perms);
  CHECK(info.base_gva <= gva);
  CHECK(info.base_gva + info.size >= gva + size);
}

static void expect_unmapped(VMM_Context *vmm, uint64_t gva) {
  VMM_Region_Info info;
  CHECK_OK(vmm_query(vmm, gva, &info));
  CHECK(!info.is_mapped);
}

static bool guest_is_zero(VMM_Context *vmm, uint64_t gva, uint64_t size) {
  static uint8_t buffer[0x10000];
  CHECK(size <= sizeof(buffer));
  CHECK_OK(vmm_read_block(vmm, gva, buffer, size));
  for (uint64_t i = 0; i < size; i++) {
    if (buffer[i] != 0) return false;
  }
  return true;
}

static void check_module_bytes(VMM_Context *vmm, const Process_Module *module, size_t m) {
  static uint8_t buffer[MAX_SEGMENT];
  const Module_Spec *spec = &k_specs[m];
  CHECK_OK(vmm_read_block(vmm, module->text.base, buffer, spec->text_size));
  CHECK(memcmp(buffer, g_text[m], spec->text_size) == 0);
  if (spec->rodata_size) {
    CHECK_OK(vmm_read_block(vmm, module->rodata.base, buffer, spec->rodata_size));
    CHECK(memcmp(buffer, g_rodata[m], spec->rodata_size) == 0);
  }
  CHECK_OK(vmm_read_block(vmm, module->data.base, buffer, spec->data_size));
  CHECK(memcmp(buffer, g_data[m], spec->data_size) == 0);
  /* The tail of the text page past .text, and all of .bss, read as zero. */
  const uint64_t text_end = module->text.base + spec->text_size;
  CHECK(guest_is_zero(vmm, text_end, module->text.base + module->text.size - text_end));
  const uint64_t bss = module->data.base + spec->data_size;
  CHECK(guest_is_zero(vmm, bss, module->base_gva + module->image_size - bss));
}

static void check_bootstrapped(const Process *process, VMM_Context *vmm, size_t module_count,
                               uint64_t expected_code_base) {
  const Address_Space *as = &process->address_space;
  CHECK(process->module_count == module_count);
  CHECK(as->code.base == expected_code_base);
  uint64_t cursor = as->code.base;
  uint64_t total = 0;
  for (size_t m = 0; m < module_count; m++) {
    const Process_Module *module = &process->modules[m];
    CHECK(strcmp(module->name, k_specs[m].name) == 0);
    CHECK(module->base_gva == cursor);
    CHECK(module->image_size == k_specs[m].expected_image_size);
    uint8_t expected_id[0x20];
    memset(expected_id, k_specs[m].text_fill, sizeof(expected_id));
    CHECK(memcmp(module->module_id, expected_id, sizeof(expected_id)) == 0);

    expect_run(vmm, module->text.base, module->text.size, VMM_PERM_RX);
    if (module->rodata.size) expect_run(vmm, module->rodata.base, module->rodata.size, VMM_PERM_R);
    expect_run(vmm, module->data.base, module->data.size, VMM_PERM_RW);
    CHECK(module->data.base + module->data.size == module->base_gva + module->image_size);
    check_module_bytes(vmm, module, m);

    cursor += module->image_size;
    total += module->image_size;
  }
  CHECK(as->code.size == total);
  CHECK(process->code_bytes_mapped == total);
  CHECK(process->entry_point == process->modules[0].base_gva);
  expect_unmapped(vmm, cursor); /* nothing past the last module */

  /* Stack: guard page unmapped, then RW, then unmapped again. */
  CHECK(process->main_thread_stack.base == as->stack.base + PAGE);
  CHECK(process->main_thread_stack.size == STACK_BYTES);
  expect_unmapped(vmm, as->stack.base);
  expect_run(vmm, process->main_thread_stack.base, STACK_BYTES, VMM_PERM_RW);
  expect_unmapped(vmm, process->main_thread_stack.base + STACK_BYTES);
  CHECK(guest_is_zero(vmm, process->main_thread_stack.base, 0x1000));

  /* TLS: one RW page at the start of tls_io; block 0 is the main thread's. */
  CHECK(process->tls_page_gva == as->tls_io.base);
  CHECK(process->main_thread_tls_gva == process->tls_page_gva);
  CHECK((process->main_thread_tls_gva % PROCESS_TLS_BLOCK_BYTES) == 0);
  CHECK(process->tls_blocks_used == 1);
  expect_run(vmm, process->tls_page_gva, PAGE, VMM_PERM_RW);
  expect_unmapped(vmm, process->tls_page_gva + PAGE);
  CHECK(guest_is_zero(vmm, process->tls_page_gva, PAGE));

  CHECK(process->main_thread_handle == PROCESS_MAIN_THREAD_HANDLE);
  CHECK(strcmp(process->npdm.name, "ProcTest") == 0);
}

int main(void) {
  build_module_images();
  build_npdm(3); /* 39-bit */

  Emulator emu;
  CHECK_OK(emulator_create(&emu));
  Arena scratch;
  CHECK(arena_create(&scratch, SCRATCH_BYTES));
  Arena exefs_arena;
  CHECK(arena_create(&exefs_arena, SCRATCH_BYTES));

  /* Physical pages are not zeroed by the allocator (page_allocator.h),
   * and fresh RAM happens to be zero - so dirty the pages the bootstrap
   * will hand out, or the .bss/stack/TLS zeroing checks below prove
   * nothing. */
  {
    static uint8_t junk[0x10000];
    memset(junk, 0xFF, sizeof(junk));
    /* Modules (~0x12000) + stack (STACK_BYTES) + TLS page all come from
     * the first megabyte of the allocator; dirty all of it. */
    const uint64_t dirty_bytes = 0x100000;
    const uint64_t dirty_gva = 0x4000000000ull;
    CHECK_OK(vmm_map(emu.vmm, dirty_gva, 0, dirty_bytes, VMM_PERM_RW));
    for (uint64_t at = 0; at < dirty_bytes; at += sizeof(junk)) {
      CHECK_OK(vmm_write_block(emu.vmm, dirty_gva + at, junk, sizeof(junk)));
    }
    CHECK_OK(vmm_unmap(emu.vmm, dirty_gva, dirty_bytes));
  }

  /* --- The happy path: four modules, no ASLR. --- */
  Fixture_Buffer exefs_image;
  build_exefs(SPEC_COUNT, &exefs_image);
  Byte_Source exefs_source = byte_source_from_memory(exefs_image.bytes, exefs_image.size);
  ExeFS exefs;
  CHECK_OK(exefs_open(&exefs_source, &exefs_arena, &exefs));
  const ExeFS_Entry *npdm_entry = exefs_find(&exefs, EXEFS_FILE_NPDM);
  CHECK(npdm_entry != NULL);
  NPDM npdm;
  CHECK_OK(npdm_parse(g_npdm_image.bytes, g_npdm_image.size, &npdm));

  /* Something already in the scratch arena must survive the bootstrap. */
  uint8_t *sentinel = ARENA_ALLOC_ARRAY(&scratch, uint8_t, 64);
  memset(sentinel, 0x5A, 64);
  const size_t scratch_mark = scratch.used_bytes;

  Process_Bootstrap_Params params = {
      .exefs = &exefs, .npdm = &npdm, .vmm = emu.vmm, .pages = &emu.pages,
      .scratch = &scratch, .aslr_seed = 0,
  };
  Process process;
  CHECK_OK(process_bootstrap(&params, &process));
  check_bootstrapped(&process, emu.vmm, SPEC_COUNT, ADDRESS_SPACE_39_START);
  CHECK(scratch.used_bytes == scratch_mark);
  CHECK(sentinel[0] == 0x5A && sentinel[63] == 0x5A);

  /* Main-thread entry ABI, observed through the backend. */
  const CPU_Backend *cpu = emu.cpu_backend;
  cpu->set_reg(emu.cpu_state, 5, 0xDEADBEEF);
  cpu->set_pstate(emu.cpu_state, 0xF0000000u);
  CHECK_OK(process_enter_main_thread(&process, cpu, emu.cpu_state));
  const CPU_Register_File *regs = cpu->get_register_file(emu.cpu_state);
  CHECK(regs->x[0] == 0);
  CHECK(regs->x[1] == PROCESS_MAIN_THREAD_HANDLE);
  CHECK(regs->x[5] == 0);
  CHECK(regs->x[30] == 0);
  CHECK(regs->sp == process.main_thread_stack.base + STACK_BYTES);
  CHECK(regs->pc == process.entry_point);
  CHECK(regs->pstate == 0);
  CHECK(cpu->get_sys_reg(emu.cpu_state, CPU_SYSREG_TPIDRRO_EL0) == process.main_thread_tls_gva);
  CHECK_CODE(process_enter_main_thread(NULL, cpu, emu.cpu_state), RESULT_INVALID_ARGUMENT);

  /* Lookup. */
  CHECK(process_find_module(&process, process.entry_point) == &process.modules[0]);
  CHECK(process_find_module(&process, process.modules[1].base_gva + 0x8FFF) == &process.modules[1]);
  CHECK(process_find_module(&process, process.modules[1].base_gva + 0x9000) == &process.modules[2]);
  CHECK(process_find_module(&process, 0) == NULL);
  CHECK(process_find_module(&process, process.main_thread_stack.base) == NULL);

  /* Teardown leaves vmm clean; a fresh bootstrap at the same base works. */
  const uint64_t old_stack = process.main_thread_stack.base;
  const uint64_t old_tls = process.tls_page_gva;
  process_teardown(&process, emu.vmm);
  CHECK(process.module_count == 0);
  expect_unmapped(emu.vmm, ADDRESS_SPACE_39_START);
  expect_unmapped(emu.vmm, old_stack);
  expect_unmapped(emu.vmm, old_tls);
  page_allocator_reset(&emu.pages);
  CHECK_OK(process_bootstrap(&params, &process));
  check_bootstrapped(&process, emu.vmm, SPEC_COUNT, ADDRESS_SPACE_39_START);
  process_teardown(&process, emu.vmm);
  page_allocator_reset(&emu.pages);

  /* --- ASLR: the seed moves the code base by whole granules. --- */
  params.aslr_seed = 0xC0FFEE;
  CHECK_OK(process_bootstrap(&params, &process));
  CHECK(process.entry_point != ADDRESS_SPACE_39_START);
  CHECK((process.entry_point - ADDRESS_SPACE_39_START) % ADDRESS_SPACE_ASLR_GRANULE == 0);
  check_bootstrapped(&process, emu.vmm, SPEC_COUNT, process.entry_point);
  process_teardown(&process, emu.vmm);
  page_allocator_reset(&emu.pages);
  params.aslr_seed = 0;

  /* --- Rollback: pages run out on the second module. --- */
  Page_Allocator small_pool;
  CHECK_OK(page_allocator_init(&small_pool, 0x10000000, 6 * PAGE)); /* rtld needs 4, main 9 */
  params.pages = &small_pool;
  CHECK_CODE(process_bootstrap(&params, &process), RESULT_OUT_OF_MEMORY);
  expect_unmapped(emu.vmm, ADDRESS_SPACE_39_START);
  expect_unmapped(emu.vmm, ADDRESS_SPACE_39_START + k_specs[0].expected_image_size);
  CHECK(scratch.used_bytes == scratch_mark);
  CHECK(process.module_count == 0);
  params.pages = &emu.pages;

  /* --- Rejections that never map anything. --- */
  CHECK_CODE(process_bootstrap(NULL, &process), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(process_bootstrap(&params, NULL), RESULT_INVALID_ARGUMENT);
  Process_Bootstrap_Params bad = params;
  bad.scratch = NULL;
  CHECK_CODE(process_bootstrap(&bad, &process), RESULT_INVALID_ARGUMENT);

  /* No `main`: an ExeFS with npdm + rtld only. */
  Fixture_Buffer no_main_image;
  build_exefs(1, &no_main_image);
  Byte_Source no_main_source = byte_source_from_memory(no_main_image.bytes, no_main_image.size);
  ExeFS no_main;
  CHECK_OK(exefs_open(&no_main_source, &exefs_arena, &no_main));
  bad = params;
  bad.exefs = &no_main;
  CHECK_CODE(process_bootstrap(&bad, &process), RESULT_NOT_FOUND);
  expect_unmapped(emu.vmm, ADDRESS_SPACE_39_START);

  /* A corrupt NSO (magic) in an otherwise good ExeFS. */
  {
    const ExeFS_Entry *sdk_entry = exefs_find(&exefs, "sdk");
    CHECK(sdk_entry != NULL);
    exefs_image.bytes[sdk_entry->offset] = 'X';
    CHECK_CODE(process_bootstrap(&params, &process), RESULT_INVALID_ARGUMENT);
    expect_unmapped(emu.vmm, ADDRESS_SPACE_39_START);
    exefs_image.bytes[sdk_entry->offset] = 'N';
  }

  /* Unsupported address space (36-bit npdm). */
  build_npdm(1);
  NPDM npdm36;
  CHECK_OK(npdm_parse(g_npdm_image.bytes, g_npdm_image.size, &npdm36));
  bad = params;
  bad.npdm = &npdm36;
  CHECK_CODE(process_bootstrap(&bad, &process), RESULT_NOT_IMPLEMENTED);
  expect_unmapped(emu.vmm, ADDRESS_SPACE_39_START);

  /* An unaligned stack size never reaches the address space. */
  build_npdm(3);
  NPDM npdm_bad_stack;
  CHECK_OK(npdm_parse(g_npdm_image.bytes, g_npdm_image.size, &npdm_bad_stack));
  npdm_bad_stack.main_thread_stack_size = 0x1234;
  bad.npdm = &npdm_bad_stack;
  CHECK_CODE(process_bootstrap(&bad, &process), RESULT_INVALID_ARGUMENT);

  /* A stack the size of the whole stack region cannot fit behind its
   * guard page: rejected against the layout, before any mapping, with the
   * modules already mapped and then unwound. */
  npdm_bad_stack.main_thread_stack_size = (uint32_t)ADDRESS_SPACE_39_STACK_SIZE;
  {
    const Error err = process_bootstrap(&bad, &process);
    CHECK_CODE(err, RESULT_INVALID_ARGUMENT);
    CHECK(strstr(err.message, "stack region") != NULL);
  }
  expect_unmapped(emu.vmm, ADDRESS_SPACE_39_START);
  CHECK(process.module_count == 0);

  /* A module whose .text sits at a non-zero (page-aligned) offset: the
   * entry point is the .text base, which is executable - not the RW
   * image base. */
  {
    static uint8_t offset_text[0x300];
    for (size_t i = 0; i < sizeof(offset_text); i++) offset_text[i] = (uint8_t)(0x77 + i);
    Fixture_NSO_Params p;
    memset(&p, 0, sizeof(p));
    p.text = (Fixture_NSO_Segment){offset_text, sizeof(offset_text), PAGE, true, false};
    p.rodata = (Fixture_NSO_Segment){"", 0, 2 * PAGE, false, false};
    p.data = (Fixture_NSO_Segment){g_data[0], 0x10, 2 * PAGE, false, false};
    Fixture_Buffer offset_nso;
    fixture_build_nso(&p, &offset_nso);
    const Fixture_File files[] = {
        {EXEFS_FILE_NPDM, g_npdm_image.bytes, g_npdm_image.size},
        {EXEFS_FILE_MAIN, offset_nso.bytes, offset_nso.size},
    };
    Fixture_Buffer offset_image;
    fixture_build_pfs0(files, 2, &offset_image);
    Byte_Source offset_source = byte_source_from_memory(offset_image.bytes, offset_image.size);
    ExeFS offset_exefs;
    CHECK_OK(exefs_open(&offset_source, &exefs_arena, &offset_exefs));
    Process_Bootstrap_Params offset_params = params;
    offset_params.exefs = &offset_exefs;
    CHECK_OK(process_bootstrap(&offset_params, &process));
    CHECK(process.modules[0].text.base == process.modules[0].base_gva + PAGE);
    CHECK(process.entry_point == process.modules[0].text.base);
    expect_run(emu.vmm, process.entry_point, PAGE, VMM_PERM_RX);
    expect_run(emu.vmm, process.modules[0].base_gva, PAGE, VMM_PERM_RW); /* the hole before .text */
    CHECK_OK(process_enter_main_thread(&process, cpu, emu.cpu_state));
    CHECK(cpu->get_pc(emu.cpu_state) == process.modules[0].base_gva + PAGE);
    process_teardown(&process, emu.vmm);
    page_allocator_reset(&emu.pages);
    fixture_buffer_free(&offset_image);
    fixture_buffer_free(&offset_nso);
  }

  /* Still bootstrappable after all of that: nothing leaked into vmm. */
  CHECK_OK(process_bootstrap(&params, &process));
  check_bootstrapped(&process, emu.vmm, SPEC_COUNT, ADDRESS_SPACE_39_START);
  process_teardown(&process, emu.vmm);

  arena_destroy(&exefs_arena);
  arena_destroy(&scratch);
  emulator_destroy(&emu);
  fixture_buffer_free(&no_main_image);
  fixture_buffer_free(&exefs_image);
  for (size_t m = 0; m < SPEC_COUNT; m++) fixture_buffer_free(&g_nso_images[m]);
  fixture_buffer_free(&g_npdm_image);
  printf("[process_test] passed\n");
  return 0;
}
