/**
 * core/hle/kernel/svc_ipc unit test: ConnectToNamedPort, SendSyncRequest,
 * CloseHandle driven through hle_on_svc the way svc_memory_test.c drives
 * the memory SVCs - registers set by hand, state asserted through the
 * CPU backend, vmm, and (white-box, like svc_memory_test's
 * emu.process.heap_size checks) emu.hle.sessions directly.
 */
#define CHECK_NAME "svc_ipc_test"
#include "check.h"

#include "common/arena.h"
#include "emulator.h"
#include "hle/hle.h"
#include "hle/kernel/svc_ipc.h"
#include "loader_fixtures.h"

#include <string.h>

#define PAGE VMM_PAGE_SIZE
#define STACK_BYTES 0x80000u
#define PROGRAM_ID 0x0100000000044000ull

static const uint32_t k_caps[] = {0x7u | (44u << 4) | (28u << 10) | (0u << 16) | (2u << 24),
                                  0xFu | (0xFFu << 5)};

/* Same minimal single-module bootstrap as svc_memory_test.c - this test
 * is not re-testing the loader. */
static void bootstrap_minimal(Emulator *emu) {
  static uint8_t text[PAGE];
  memset(text, 0xC3, sizeof(text));
  Fixture_NSO_Params nso_params;
  memset(&nso_params, 0, sizeof(nso_params));
  nso_params.text = (Fixture_NSO_Segment){text, sizeof(text), 0, false, false};
  static const uint8_t empty_segment[1] = {0};
  nso_params.rodata = (Fixture_NSO_Segment){empty_segment, 0, (uint32_t)sizeof(text), false, false};
  nso_params.data = (Fixture_NSO_Segment){empty_segment, 0, (uint32_t)sizeof(text), false, false};
  uint8_t module_id[0x20];
  memset(module_id, 0xCD, sizeof(module_id));
  nso_params.module_id = module_id;
  Fixture_Buffer nso_image;
  fixture_buffer_init(&nso_image);
  fixture_build_nso(&nso_params, &nso_image);

  Fixture_NPDM_Params npdm_params;
  memset(&npdm_params, 0, sizeof(npdm_params));
  npdm_params.flags = (uint8_t)(0x01 | (3u << 1));
  npdm_params.main_thread_priority = 44;
  npdm_params.main_thread_stack_size = STACK_BYTES;
  npdm_params.name = "SvcIpcTest";
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
  CHECK_OK(process_bootstrap(&params, &emu->process));

  arena_destroy(&exefs_arena);
  arena_destroy(&scratch);
  fixture_buffer_free(&nso_image);
  fixture_buffer_free(&npdm_image);
  fixture_buffer_free(&exefs_image);
}

static void write_u32_at(uint8_t *buf, uint32_t offset, uint32_t v) { memcpy(buf + offset, &v, 4); }
static void write_u64_at(uint8_t *buf, uint32_t offset, uint64_t v) { memcpy(buf + offset, &v, 8); }
static uint32_t read_u32_at(const uint8_t *buf, uint32_t offset) {
  uint32_t v;
  memcpy(&v, buf + offset, 4);
  return v;
}
static uint64_t read_u64_at(const uint8_t *buf, uint32_t offset) {
  uint64_t v;
  memcpy(&v, buf + offset, 8);
  return v;
}

/* Mirrors ipc_test.c's build_request and ipc_response_write's own wire
 * format exactly - see that file's header comment. */
static void build_command_buffer(uint8_t *buf, uint32_t command_id, const void *payload,
                                 uint32_t payload_size) {
  memset(buf, 0, TLS_IPC_COMMAND_BUFFER_BYTES);
  const uint32_t raw_offset = (8u + 15u) & ~15u;
  const uint32_t cmif_bytes = 16u + payload_size;
  write_u32_at(buf, raw_offset, 0x49434653u); /* "SFCI" */
  write_u32_at(buf, raw_offset + 4, 0);
  write_u64_at(buf, raw_offset + 8, command_id);
  if (payload_size > 0) memcpy(buf + raw_offset + 16, payload, payload_size);
  write_u32_at(buf, 0, 4 /* IPC_MESSAGE_TYPE_REQUEST */);
  write_u32_at(buf, 4, (cmif_bytes / 4u) & 0x3FFu);
}

static const Service_Interface k_fake_fsp_interface = {
    .name = "fsp-srv", .commands = NULL, .command_count = 0};

int main(void) {
  Emulator emu;
  bootstrap_minimal(&emu);
  CPU_Register_File *regs = emu.cpu_backend->get_register_file(emu.cpu_state);
  const uint64_t scratch_gva = emu.process.main_thread_stack.base; /* mapped RW, unused by the test */

  /* --- ConnectToNamedPort --- */

  CHECK_OK(vmm_write_block(emu.vmm, scratch_gva, "sm:", 4));
  regs->x[1] = scratch_gva;
  hle_on_svc(emu.cpu_state, 0x1F, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
  const uint32_t sm_handle = (uint32_t)regs->x[1];
  CHECK(sm_handle != IPC_INVALID_HANDLE);

  CHECK_OK(vmm_write_block(emu.vmm, scratch_gva, "xx:", 4));
  regs->x[1] = scratch_gva;
  hle_on_svc(emu.cpu_state, 0x1F, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_NOT_FOUND);

  /* --- SendSyncRequest: RegisterClient (explicit stub, sm.h) --- */

  uint8_t cmd_buf[TLS_IPC_COMMAND_BUFFER_BYTES];
  build_command_buffer(cmd_buf, SM_CMD_REGISTER_CLIENT, NULL, 0);
  CHECK_OK(vmm_write_block(emu.vmm, emu.process.main_thread_tls_gva, cmd_buf, sizeof(cmd_buf)));
  regs->x[0] = sm_handle;
  hle_on_svc(emu.cpu_state, 0x21, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_SUCCESS);

  uint8_t response[TLS_IPC_COMMAND_BUFFER_BYTES];
  CHECK_OK(vmm_read_block(emu.vmm, emu.process.main_thread_tls_gva, response, sizeof(response)));
  const uint32_t raw_offset = (8u + 15u) & ~15u;
  CHECK(read_u32_at(response, raw_offset) == 0x4F434653u); /* "SFCO" */
  CHECK(read_u64_at(response, raw_offset + 8) == HLE_RESULT_SUCCESS);

  /* --- SendSyncRequest: GetServiceHandle, registered name --- */

  int fake_instance_marker = 0;
  uint8_t fsp_name[SM_SERVICE_NAME_BYTES];
  memset(fsp_name, 0, sizeof(fsp_name));
  memcpy(fsp_name, "fsp-srv", 7);
  CHECK_OK(sm_register_service(&emu.sm, fsp_name, &k_fake_fsp_interface, &fake_instance_marker));

  build_command_buffer(cmd_buf, SM_CMD_GET_SERVICE, fsp_name, sizeof(fsp_name));
  CHECK_OK(vmm_write_block(emu.vmm, emu.process.main_thread_tls_gva, cmd_buf, sizeof(cmd_buf)));
  regs->x[0] = sm_handle;
  hle_on_svc(emu.cpu_state, 0x21, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_SUCCESS);

  CHECK_OK(vmm_read_block(emu.vmm, emu.process.main_thread_tls_gva, response, sizeof(response)));
  CHECK(read_u64_at(response, raw_offset + 8) == HLE_RESULT_SUCCESS);
  const uint32_t special = read_u32_at(response, 8);
  CHECK(((special >> 5) & 0xFu) == 1); /* one move handle */
  const uint32_t fsp_handle = read_u32_at(response, 12);
  CHECK(fsp_handle != IPC_INVALID_HANDLE);

  /* White-box, like svc_memory_test's emu.process.heap_size checks: the
   * handle really resolves to the registered interface/instance. */
  IPC_Session *fsp_session = ipc_session_table_get(&emu.hle.sessions, fsp_handle);
  CHECK(fsp_session != NULL);
  CHECK(fsp_session->object.interface == &k_fake_fsp_interface);
  CHECK(fsp_session->object.instance == &fake_instance_marker);

  /* --- SendSyncRequest: GetServiceHandle, unregistered name --- */

  uint8_t bogus_name[SM_SERVICE_NAME_BYTES];
  memset(bogus_name, 0, sizeof(bogus_name));
  memcpy(bogus_name, "nope-srv", 8);
  build_command_buffer(cmd_buf, SM_CMD_GET_SERVICE, bogus_name, sizeof(bogus_name));
  CHECK_OK(vmm_write_block(emu.vmm, emu.process.main_thread_tls_gva, cmd_buf, sizeof(cmd_buf)));
  regs->x[0] = sm_handle;
  hle_on_svc(emu.cpu_state, 0x21, &emu.hle);
  /* SendSyncRequest itself succeeds (the IPC mechanism delivered the
   * call); the service's own failure lives in the CMIF response, not in
   * this SVC's Result - real Horizon semantics (svc_ipc.h). */
  CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
  CHECK_OK(vmm_read_block(emu.vmm, emu.process.main_thread_tls_gva, response, sizeof(response)));
  CHECK(read_u64_at(response, raw_offset + 8) == SM_RESULT_NOT_FOUND);

  /* --- SendSyncRequest: unimplemented command (RegisterService et al.) --- */

  build_command_buffer(cmd_buf, 2 /* RegisterService: not implemented, sm.h */, NULL, 0);
  CHECK_OK(vmm_write_block(emu.vmm, emu.process.main_thread_tls_gva, cmd_buf, sizeof(cmd_buf)));
  regs->x[0] = sm_handle;
  hle_on_svc(emu.cpu_state, 0x21, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
  CHECK_OK(vmm_read_block(emu.vmm, emu.process.main_thread_tls_gva, response, sizeof(response)));
  CHECK(read_u64_at(response, raw_offset + 8) == HLE_RESULT_NOT_IMPLEMENTED);

  /* --- SendSyncRequest with an invalid handle --- */

  regs->x[0] = 0xFFFFu;
  hle_on_svc(emu.cpu_state, 0x21, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_INVALID_HANDLE);

  /* --- CloseHandle --- */

  regs->x[0] = fsp_handle;
  hle_on_svc(emu.cpu_state, 0x26, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_SUCCESS);
  CHECK(ipc_session_table_get(&emu.hle.sessions, fsp_handle) == NULL);

  regs->x[0] = fsp_handle; /* already closed */
  hle_on_svc(emu.cpu_state, 0x26, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_INVALID_HANDLE);

  emulator_destroy(&emu);
  printf("[" CHECK_NAME "] all checks passed\n");
  return 0;
}
