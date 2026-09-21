/**
 * Loader integration path: the §12 bootstrap's first two steps, driven
 * end to end over a synthesized PROGRAM NCA -
 *
 *   nca_open -> nca_probe_section -> exefs_open -> exefs_find(main.npdm)
 *            -> npdm_parse,
 *   nca_open -> nca_probe_section -> exefs_open -> exefs_entry_source(main)
 *            -> nso_open -> nso_read_segment,
 *   nca_open -> nca_probe_section -> romfs_open -> romfs_find_file, and
 *   emulator_load_program over the same NCA: the whole of §12 steps 1-4
 *   ending in an armed main-thread register file.
 *
 * Each parser has its own unit test; this proves the seams between them
 * (section slices feeding the next parser, arena sharing, error codes
 * surviving the composition).
 */
#define CHECK_NAME "loader_smoke"
#include "check.h"

#include "common/arena.h"
#include "emulator.h"
#include "hle/loader/exefs.h"
#include "hle/loader/nca_parse.h"
#include "hle/loader/npdm.h"
#include "hle/loader/nso.h"
#include "hle/loader/romfs.h"
#include "loader_fixtures.h"

#include <string.h>

#define ARENA_BYTES (1024 * 1024)
#define PROGRAM_ID 0x0100000000042000ull

static const uint32_t k_caps[] = {0x7u | (44u << 4) | (28u << 10) | (0u << 16) | (2u << 24),
                                  0xFu | (0xFFu << 5)};
static uint8_t k_rtld_text[0x200];
static uint8_t k_main_text[0x1000 + 40];
static const char k_main_data[] = "main-data-segment";
static const char k_texture[] = "texture-bytes";

int main(void) {
  /* Build main.npdm, wrap it and two NSO stand-ins in a PFS0, put a RomFS
   * beside it, and wrap both sections in an NCA. */
  Fixture_NPDM_Params npdm_params;
  memset(&npdm_params, 0, sizeof(npdm_params));
  npdm_params.flags = 0x01 | (3 << 1);
  npdm_params.main_thread_priority = 44;
  npdm_params.main_thread_stack_size = 0x80000;
  npdm_params.name = "Smoke";
  npdm_params.program_id_min = PROGRAM_ID;
  npdm_params.program_id_max = PROGRAM_ID;
  npdm_params.acid_capabilities = k_caps;
  npdm_params.acid_capability_count = 2;
  npdm_params.program_id = PROGRAM_ID;
  npdm_params.aci0_capabilities = k_caps;
  npdm_params.aci0_capability_count = 2;
  Fixture_Buffer npdm_image;
  fixture_build_npdm(&npdm_params, &npdm_image);

  /* `rtld`: a tiny NSO (.text only); `main`: a compressed .text and a
   * raw .data. */
  for (size_t i = 0; i < sizeof(k_rtld_text); i++) k_rtld_text[i] = (uint8_t)(0xD4 + i % 5);
  Fixture_NSO_Params rtld_params;
  memset(&rtld_params, 0, sizeof(rtld_params));
  rtld_params.text = (Fixture_NSO_Segment){k_rtld_text, sizeof(k_rtld_text), 0, true, false};
  rtld_params.rodata = (Fixture_NSO_Segment){"", 0, 0x1000, false, false};
  rtld_params.data = (Fixture_NSO_Segment){"", 0, 0x1000, false, false};
  Fixture_Buffer rtld_nso_image;
  fixture_build_nso(&rtld_params, &rtld_nso_image);

  for (size_t i = 0; i < sizeof(k_main_text); i++) k_main_text[i] = (uint8_t)(i % 17);
  Fixture_NSO_Params nso_params;
  memset(&nso_params, 0, sizeof(nso_params));
  nso_params.text = (Fixture_NSO_Segment){k_main_text, sizeof(k_main_text), 0, true, true};
  nso_params.rodata = (Fixture_NSO_Segment){"", 0, 0x2000, false, false};
  nso_params.data = (Fixture_NSO_Segment){k_main_data, sizeof(k_main_data), 0x2000, false, false};
  nso_params.bss_size = 0x100;
  Fixture_Buffer main_nso_image;
  fixture_build_nso(&nso_params, &main_nso_image);

  const Fixture_File exefs_files[] = {
      {EXEFS_FILE_NPDM, npdm_image.bytes, npdm_image.size},
      {EXEFS_FILE_RTLD, rtld_nso_image.bytes, rtld_nso_image.size},
      {EXEFS_FILE_MAIN, main_nso_image.bytes, main_nso_image.size},
  };
  Fixture_Buffer exefs_image;
  fixture_build_pfs0(exefs_files, 3, &exefs_image);

  const Fixture_RomFS_Entry romfs_entries[] = {
      {"/textures", true, NULL, 0},
      {"/textures/sky.tex", false, k_texture, sizeof(k_texture) - 1},
  };
  Fixture_Buffer romfs_image;
  fixture_build_romfs(romfs_entries, 2, 0, &romfs_image);

  Fixture_NCA_Section sections[4];
  memset(sections, 0, sizeof(sections));
  sections[0] = (Fixture_NCA_Section){true, FIXTURE_NCA_FS_TYPE_PFS0, FIXTURE_NCA_HASH_SHA256, 3, &exefs_image, 0x200};
  sections[1] = (Fixture_NCA_Section){true, FIXTURE_NCA_FS_TYPE_ROMFS, FIXTURE_NCA_HASH_IVFC, 3, &romfs_image, 0x200};
  Fixture_Buffer nca_image;
  fixture_build_nca(NCA_MAGIC_NCA3, 0, PROGRAM_ID, sections, &nca_image);

  /* --- The bootstrap path. --- */
  Byte_Source file = byte_source_from_memory(nca_image.bytes, nca_image.size);
  Arena arena;
  CHECK(arena_create(&arena, ARENA_BYTES));

  NCA_File nca;
  CHECK_OK(nca_open(&file, &nca));
  CHECK(nca.header.content_type == NCA_CONTENT_PROGRAM);
  CHECK(nca.header.program_id == PROGRAM_ID);

  const int exefs_index = nca_find_section(&nca, NCA_FS_PARTITION_FS);
  CHECK(exefs_index == 0);
  CHECK_OK(nca_probe_section(&nca, (uint32_t)exefs_index));
  ExeFS exefs;
  CHECK_OK(exefs_open(nca_section_source(&nca, (uint32_t)exefs_index), &arena, &exefs));
  CHECK(exefs.file_count == 3);

  const ExeFS_Entry *npdm_entry = exefs_find(&exefs, EXEFS_FILE_NPDM);
  CHECK(npdm_entry != NULL);
  uint8_t *npdm_bytes = ARENA_ALLOC_ARRAY(&arena, uint8_t, npdm_entry->size);
  CHECK(npdm_bytes != NULL);
  CHECK_OK(exefs_read(&exefs, npdm_entry, 0, npdm_bytes, npdm_entry->size));
  NPDM npdm;
  CHECK_OK(npdm_parse(npdm_bytes, npdm_entry->size, &npdm));
  CHECK(strcmp(npdm.name, "Smoke") == 0);
  CHECK(npdm.address_space == NPDM_ADDRESS_SPACE_64_BIT_39);
  CHECK(npdm.main_thread_stack_size == 0x80000);
  CHECK(npdm.program_id == nca.header.program_id);
  CHECK(npdm_svc_allowed(&npdm, 7) && !npdm_svc_allowed(&npdm, 8));

  /* `main` through the ExeFS slice: header, then a decompressed segment
   * staged in the shared arena, then a raw one. */
  Byte_Source_Slice main_nso_source;
  CHECK_OK(exefs_entry_source(&exefs, exefs_find(&exefs, EXEFS_FILE_MAIN), &main_nso_source));
  NSO main_nso;
  CHECK_OK(nso_open(&main_nso_source.source, &main_nso));
  CHECK(main_nso.segments[NSO_SEGMENT_TEXT].is_compressed);
  CHECK(main_nso.segments[NSO_SEGMENT_TEXT].memory_size == sizeof(k_main_text));
  CHECK(main_nso.image_size == 0x3000);
  uint8_t *text = ARENA_ALLOC_ARRAY(&arena, uint8_t, sizeof(k_main_text));
  CHECK(text != NULL);
  CHECK_OK(nso_read_segment(&main_nso, NSO_SEGMENT_TEXT, &arena, text, sizeof(k_main_text)));
  CHECK(memcmp(text, k_main_text, sizeof(k_main_text)) == 0);
  char probe[32];
  CHECK_OK(nso_read_segment(&main_nso, NSO_SEGMENT_DATA, NULL, (uint8_t *)probe, sizeof(probe)));
  CHECK(memcmp(probe, k_main_data, sizeof(k_main_data)) == 0);

  const int romfs_index = nca_find_section(&nca, NCA_FS_ROMFS);
  CHECK(romfs_index == 1);
  CHECK_OK(nca_probe_section(&nca, (uint32_t)romfs_index));
  RomFS romfs;
  CHECK_OK(romfs_open(nca_section_source(&nca, (uint32_t)romfs_index), &arena, &romfs));
  RomFS_File_Entry sky;
  CHECK_OK(romfs_find_file(&romfs, "/textures/sky.tex", &sky));
  CHECK(sky.data_size == sizeof(k_texture) - 1);
  CHECK_OK(romfs_read_file(&romfs, &sky, 0, probe, sky.data_size));
  CHECK(memcmp(probe, k_texture, sky.data_size) == 0);
  /* And through the NCA file itself: the slice chain resolves to the
   * right absolute offset. */
  CHECK(memcmp(nca_image.bytes + nca.header.sections[1].data_offset + (sky.data_offset), k_texture,
               sky.data_size) == 0);

  /* --- Steps 1-4 through the emulator: "load a game". --- */
  Emulator emu;
  CHECK_OK(emulator_create(&emu));
  CHECK(!emu.program_loaded);
  CHECK_OK(emulator_load_program(&emu, &file, 0));
  CHECK(emu.program_loaded);
  CHECK(emu.process.module_count == 2);
  CHECK(strcmp(emu.process.modules[0].name, EXEFS_FILE_RTLD) == 0);
  CHECK(strcmp(emu.process.modules[1].name, EXEFS_FILE_MAIN) == 0);
  CHECK(emu.process.entry_point == ADDRESS_SPACE_39_START);
  CHECK(emu.process.modules[1].base_gva == ADDRESS_SPACE_39_START + 0x1000);
  CHECK(strcmp(emu.process.npdm.name, "Smoke") == 0);
  /* rtld's bytes are in guest memory, executable, and the main thread
   * is parked on them. */
  uint8_t rtld_probe[sizeof(k_rtld_text)];
  CHECK_OK(vmm_read_block(emu.vmm, emu.process.entry_point, rtld_probe, sizeof(rtld_probe)));
  CHECK(memcmp(rtld_probe, k_rtld_text, sizeof(k_rtld_text)) == 0);
  VMM_Region_Info info;
  CHECK_OK(vmm_query(emu.vmm, emu.process.entry_point, &info));
  CHECK(info.is_mapped && info.perms == VMM_PERM_RX);
  const CPU_Register_File *regs = emu.cpu_backend->get_register_file(emu.cpu_state);
  CHECK(regs->pc == emu.process.entry_point);
  CHECK(regs->x[0] == 0 && regs->x[1] == PROCESS_MAIN_THREAD_HANDLE);
  CHECK(regs->sp == emu.process.main_thread_stack.base + npdm.main_thread_stack_size);
  CHECK(emu.cpu_backend->get_sys_reg(emu.cpu_state, CPU_SYSREG_TPIDRRO_EL0) ==
        emu.process.main_thread_tls_gva);
  /* The noop backend still honors the bounded-run contract on it. */
  CHECK(emulator_run(&emu, 100) == CPU_EXIT_CYCLES_ELAPSED);
  /* One program at a time; unload then reload works. */
  CHECK_CODE(emulator_load_program(&emu, &file, 0), RESULT_INVALID_ARGUMENT);
  emulator_unload_program(&emu);
  CHECK(!emu.program_loaded);
  CHECK_OK(vmm_query(emu.vmm, ADDRESS_SPACE_39_START, &info));
  CHECK(!info.is_mapped);
  CHECK(emu.pages.used_bytes == 0);
  CHECK_OK(emulator_load_program(&emu, &file, 0));
  CHECK(emu.program_loaded);

  /* --- The §1.6 user-facing path survives the composition. --- */
  memset(nca_image.bytes + FIXTURE_NCA_OFFSET_MAGIC, 0x5A, 4);
  NCA_File encrypted;
  Error err = nca_open(&file, &encrypted);
  CHECK(err.code == RESULT_ENCRYPTED_INPUT);
  CHECK(strstr(err.message, "docs/DUMP.md") != NULL);
  /* ...and through the emulator entry point, with the old program still
   * intact (the failure happens before anything is touched). */
  emulator_unload_program(&emu);
  err = emulator_load_program(&emu, &file, 0);
  CHECK(err.code == RESULT_ENCRYPTED_INPUT);
  CHECK(strstr(err.message, "docs/DUMP.md") != NULL);
  CHECK(!emu.program_loaded);
  emulator_destroy(&emu);

  arena_destroy(&arena);
  fixture_buffer_free(&nca_image);
  fixture_buffer_free(&romfs_image);
  fixture_buffer_free(&exefs_image);
  fixture_buffer_free(&main_nso_image);
  fixture_buffer_free(&rtld_nso_image);
  fixture_buffer_free(&npdm_image);
  printf("[loader_smoke] passed\n");
  return 0;
}
