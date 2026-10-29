/**
 * core/hle/kernel/svc_ipc unit test: ConnectToNamedPort, SendSyncRequest,
 * CloseHandle driven through hle_on_svc the way svc_memory_test.c drives
 * the memory SVCs - registers set by hand, state asserted through the
 * CPU backend, vmm, and (white-box, like svc_memory_test's
 * emu.process.heap_size checks) emu.hle.sessions directly.
 *
 * Requests are libnx's own bytes (ipc_fixtures.h) and responses are read
 * back with libnx's reading logic, so neither side reuses ipc.c's
 * understanding of the wire format. Guest-visible Results are asserted
 * against literal values derived from libnx's result.h, not against
 * hle.h's constants.
 */
#define CHECK_NAME "svc_ipc_test"
#include "check.h"

#include "common/arena.h"
#include "emulator.h"
#include "hle/hle.h"
#include "hle/kernel/svc_ipc.h"
#include "hle/loader/nca_parse.h"
#include "ipc_fixtures.h"
#include "loader_fixtures.h"

#include <string.h>

#define PAGE VMM_PAGE_SIZE
#define STACK_BYTES 0x80000u
#define PROGRAM_ID 0x0100000000044000ull
#define LOAD_CYCLES 40u /* > IPC_MAX_SESSIONS: catches sessions surviving unload */

/* SVC ids, libnx svc.s. */
#define SVC_CLOSE_HANDLE 0x16u
#define SVC_CONNECT_TO_NAMED_PORT 0x1Fu
#define SVC_SEND_SYNC_REQUEST 0x21u

/* (description << 9) | module, descriptions from libnx result.h. */
#define EXPECT_KERNEL_OUT_OF_SESSIONS 0x0E01u    /* (7 << 9) | 1 */
#define EXPECT_KERNEL_NOT_IMPLEMENTED 0x4201u    /* (33 << 9) | 1 */
#define EXPECT_KERNEL_INVALID_ADDRESS 0xCC01u    /* (102 << 9) | 1 */
#define EXPECT_KERNEL_INVALID_HANDLE 0xE401u     /* (114 << 9) | 1 */
#define EXPECT_KERNEL_OUT_OF_RANGE 0xEE01u       /* (119 << 9) | 1 */
#define EXPECT_KERNEL_NOT_FOUND 0xF201u          /* (121 << 9) | 1 */
#define EXPECT_KERNEL_CONNECTION_CLOSED 0xF601u  /* (123 << 9) | 1 */
#define EXPECT_SM_NOT_REGISTERED 0x0E15u         /* (7 << 9) | 21 */

static const uint32_t k_caps[] = {0x7u | (44u << 4) | (28u << 10) | (0u << 16) | (2u << 24),
                                  0xFu | (0xFFu << 5)};

/* A minimal single-module program wrapped in an NCA, so the test goes
 * through emulator_load_program/emulator_unload_program - the lifecycle
 * the session table has to follow. Not re-testing the loader. */
static Fixture_Buffer build_program_nca(void) {
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

  Fixture_NCA_Section sections[4];
  memset(sections, 0, sizeof(sections));
  sections[0] = (Fixture_NCA_Section){true, FIXTURE_NCA_FS_TYPE_PFS0, FIXTURE_NCA_HASH_SHA256, 3,
                                      &exefs_image, 0x200};
  Fixture_Buffer nca_image;
  fixture_buffer_init(&nca_image);
  fixture_build_nca(NCA_MAGIC_NCA3, 0, PROGRAM_ID, sections, &nca_image);

  fixture_buffer_free(&nso_image);
  fixture_buffer_free(&npdm_image);
  fixture_buffer_free(&exefs_image);
  return nca_image;
}

typedef struct Test_Env {
  Emulator emu;
  CPU_Register_File *regs;
  uint64_t scratch_gva; /* mapped RW, unused by the program */
} Test_Env;

static void load(Test_Env *env, const Byte_Source *nca) {
  CHECK_OK(emulator_load_program(&env->emu, nca, 0));
  env->scratch_gva = env->emu.process.main_thread_stack.base;
}

static uint32_t sessions_in_use(const Emulator *emu) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < IPC_MAX_SESSIONS; i++) count += emu->hle.sessions.in_use[i] ? 1u : 0u;
  return count;
}

static void svc(Test_Env *env, uint32_t id) { hle_on_svc(env->emu.cpu_state, id, &env->emu.hle); }

static uint32_t connect_port(Test_Env *env, const char *name) {
  CHECK_OK(vmm_write_block(env->emu.vmm, env->scratch_gva, name, strlen(name) + 1));
  env->regs->x[1] = env->scratch_gva;
  svc(env, SVC_CONNECT_TO_NAMED_PORT);
  return (uint32_t)env->regs->x[0];
}

static uint32_t connect_sm(Test_Env *env) {
  CHECK(connect_port(env, "sm:") == HLE_RESULT_SUCCESS);
  const uint32_t handle = (uint32_t)env->regs->x[1];
  CHECK(handle != IPC_INVALID_HANDLE);
  return handle;
}

/* Puts `buf` in the caller's TLS command buffer and sends it. */
static uint32_t send_buffer(Test_Env *env, uint32_t handle, const uint8_t *buf) {
  CHECK_OK(vmm_write_block(env->emu.vmm, env->emu.process.main_thread_tls_gva, buf,
                           TLS_IPC_COMMAND_BUFFER_BYTES));
  env->regs->x[0] = handle;
  svc(env, SVC_SEND_SYNC_REQUEST);
  return (uint32_t)env->regs->x[0];
}

static uint32_t send_fixture(Test_Env *env, uint32_t handle, const Fixture_IPC_Message *message) {
  uint8_t buf[TLS_IPC_COMMAND_BUFFER_BYTES];
  fixture_ipc_load(buf, message);
  return send_buffer(env, handle, buf);
}

static void read_tls(Test_Env *env, uint8_t *out) {
  CHECK_OK(vmm_read_block(env->emu.vmm, env->emu.process.main_thread_tls_gva, out,
                          TLS_IPC_COMMAND_BUFFER_BYTES));
}

static Fixture_Libnx_Response read_response(Test_Env *env) {
  uint8_t buf[TLS_IPC_COMMAND_BUFFER_BYTES];
  read_tls(env, buf);
  Fixture_Libnx_Response view;
  CHECK(fixture_libnx_read_response(buf, &view));
  return view;
}

static uint32_t close_handle(Test_Env *env, uint32_t handle) {
  env->regs->x[0] = handle;
  svc(env, SVC_CLOSE_HANDLE);
  return (uint32_t)env->regs->x[0];
}

static const Service_Interface k_fake_fsp_interface = {
    .name = "fsp-srv", .commands = NULL, .command_count = 0};
static int g_fake_instance_marker;

/* --- ConnectToNamedPort --- */

static void test_connect_to_named_port(Test_Env *env) {
  const uint32_t sm_handle = connect_sm(env);
  CHECK(close_handle(env, sm_handle) == HLE_RESULT_SUCCESS);

  /* libnx's smInitialize retries exactly on the kernel's NotFound. */
  CHECK(connect_port(env, "xx:") == EXPECT_KERNEL_NOT_FOUND);

  /* No NUL within 11 characters + NUL. */
  CHECK(connect_port(env, "abcdefghijkl") == EXPECT_KERNEL_OUT_OF_RANGE);

  env->regs->x[1] = 0; /* unmapped */
  svc(env, SVC_CONNECT_TO_NAMED_PORT);
  CHECK(env->regs->x[0] == EXPECT_KERNEL_INVALID_ADDRESS);

  /* A valid name ending right before an unmapped page: the read stops at
   * the NUL instead of reading a fixed 11 bytes into the hole. The main
   * thread stack's top is the end of a mapping. */
  const uint64_t stack_end = env->emu.process.main_thread_stack.base + STACK_BYTES;
  uint8_t probe;
  CHECK(!error_is_ok(vmm_read_block(env->emu.vmm, stack_end, &probe, 1)));
  CHECK_OK(vmm_write_block(env->emu.vmm, stack_end - 4u, "sm:", 4));
  env->regs->x[1] = stack_end - 4u;
  svc(env, SVC_CONNECT_TO_NAMED_PORT);
  CHECK(env->regs->x[0] == HLE_RESULT_SUCCESS);
  CHECK(close_handle(env, (uint32_t)env->regs->x[1]) == HLE_RESULT_SUCCESS);

  /* A full table is OutOfSessions. */
  uint32_t handles[IPC_MAX_SESSIONS];
  for (uint32_t i = 0; i < IPC_MAX_SESSIONS; i++) handles[i] = connect_sm(env);
  CHECK(connect_port(env, "sm:") == EXPECT_KERNEL_OUT_OF_SESSIONS);
  for (uint32_t i = 0; i < IPC_MAX_SESSIONS; i++) CHECK(close_handle(env, handles[i]) == 0);
  CHECK(sessions_in_use(&env->emu) == 0);
}

/* --- SendSyncRequest with libnx's own messages --- */

static void test_libnx_sm_session(Test_Env *env) {
  const uint32_t sm_handle = connect_sm(env);

  /* RegisterClient (explicit stub, sm.h). */
  CHECK(send_fixture(env, sm_handle, &k_fixture_register_client) == HLE_RESULT_SUCCESS);
  Fixture_Libnx_Response view = read_response(env);
  CHECK(view.result == HLE_RESULT_SUCCESS);
  CHECK(view.num_move_handles == 0);

  /* GetServiceHandle, registered name: one move handle that resolves to
   * the registered interface/instance. */
  uint8_t fsp_name[SM_SERVICE_NAME_BYTES] = {'f', 's', 'p', '-', 's', 'r', 'v', 0};
  CHECK_OK(sm_register_service(&env->emu.sm, fsp_name, &k_fake_fsp_interface,
                               &g_fake_instance_marker));
  CHECK(send_fixture(env, sm_handle, &k_fixture_get_service_fsp) == HLE_RESULT_SUCCESS);
  view = read_response(env);
  CHECK(view.result == HLE_RESULT_SUCCESS);
  CHECK(view.num_move_handles == 1);
  const uint32_t fsp_handle = view.move_handles[0];
  IPC_Session *fsp_session = ipc_session_table_get(&env->emu.hle.sessions, fsp_handle);
  CHECK(fsp_session != NULL);
  CHECK(fsp_session->object.interface == &k_fake_fsp_interface);
  CHECK(fsp_session->object.instance == &g_fake_instance_marker);

  /* A command the session's interface lacks: NotImplemented in the CMIF
   * Result; SendSyncRequest itself succeeded (the message was delivered). */
  CHECK(send_fixture(env, fsp_handle, &k_fixture_register_client) == HLE_RESULT_SUCCESS);
  CHECK(read_response(env).result == EXPECT_KERNEL_NOT_IMPLEMENTED);

  /* libnx's serviceClose: Close, then CloseHandle. Close writes nothing
   * back and leaves the handle open; later requests see the closed
   * server end. */
  uint8_t before[TLS_IPC_COMMAND_BUFFER_BYTES];
  fixture_ipc_load(before, &k_fixture_close);
  CHECK(send_buffer(env, fsp_handle, before) == HLE_RESULT_SUCCESS);
  uint8_t after[TLS_IPC_COMMAND_BUFFER_BYTES];
  read_tls(env, after);
  CHECK(memcmp(before, after, sizeof(before)) == 0);
  CHECK(ipc_session_table_get(&env->emu.hle.sessions, fsp_handle) != NULL);
  CHECK(send_fixture(env, fsp_handle, &k_fixture_get_service_fsp) ==
        EXPECT_KERNEL_CONNECTION_CLOSED);
  CHECK(close_handle(env, fsp_handle) == HLE_RESULT_SUCCESS);
  CHECK(ipc_session_table_get(&env->emu.hle.sessions, fsp_handle) == NULL);
  CHECK(close_handle(env, fsp_handle) == EXPECT_KERNEL_INVALID_HANDLE);

  /* GetServiceHandle, unregistered name: the service's failure is in the
   * CMIF Result, not the SVC's. */
  uint8_t buf[TLS_IPC_COMMAND_BUFFER_BYTES];
  fixture_ipc_load(buf, &k_fixture_get_service_fsp);
  memcpy(buf + FIXTURE_GET_SERVICE_DATA_OFFSET, "nope-srv", SM_SERVICE_NAME_BYTES);
  CHECK(send_buffer(env, sm_handle, buf) == HLE_RESULT_SUCCESS);
  view = read_response(env);
  CHECK(view.result == EXPECT_SM_NOT_REGISTERED);
  CHECK(view.num_move_handles == 0);

  /* An sm: command Voland does not implement (RegisterService, sm.h). */
  fixture_ipc_load(buf, &k_fixture_get_service_fsp);
  const uint32_t register_service = 2;
  memcpy(buf + FIXTURE_GET_SERVICE_DATA_OFFSET - IPC_CMIF_HEADER_BYTES + IPC_CMIF_COMMAND_ID_OFFSET,
         &register_service, sizeof(register_service));
  CHECK(send_buffer(env, sm_handle, buf) == HLE_RESULT_SUCCESS);
  CHECK(read_response(env).result == EXPECT_KERNEL_NOT_IMPLEMENTED);

  /* TIPC is deferred: rejected at the SVC, session still usable. */
  CHECK(send_fixture(env, sm_handle, &k_fixture_tipc_register_client) ==
        EXPECT_KERNEL_NOT_IMPLEMENTED);
  CHECK(send_fixture(env, sm_handle, &k_fixture_register_client) == HLE_RESULT_SUCCESS);

  /* Invalid handle. */
  CHECK(send_fixture(env, 0xFFFFu, &k_fixture_register_client) == EXPECT_KERNEL_INVALID_HANDLE);

  CHECK(close_handle(env, sm_handle) == HLE_RESULT_SUCCESS);
  CHECK(sessions_in_use(&env->emu) == 0);
}

/* --- No leaked sessions on failure paths --- */

static void test_out_object_without_free_slot(Test_Env *env) {
  /* Every slot taken, the last by the sm: session doing the lookup: the
   * out object cannot get a handle, and nothing stays allocated for it. */
  uint32_t handles[IPC_MAX_SESSIONS];
  for (uint32_t i = 0; i < IPC_MAX_SESSIONS; i++) handles[i] = connect_sm(env);
  CHECK(send_fixture(env, handles[0], &k_fixture_get_service_fsp) ==
        EXPECT_KERNEL_OUT_OF_SESSIONS);
  CHECK(sessions_in_use(&env->emu) == IPC_MAX_SESSIONS);
  for (uint32_t i = 0; i < IPC_MAX_SESSIONS; i++) CHECK(close_handle(env, handles[i]) == 0);
}

static void test_write_back_fault_closes_new_session(Test_Env *env) {
  /* The request is read from TLS, the lookup opens a session for the out
   * handle, and writing the response back faults: the guest never sees
   * that handle, so its session must be closed again. */
  const uint32_t sm_handle = connect_sm(env);
  uint8_t buf[TLS_IPC_COMMAND_BUFFER_BYTES];
  fixture_ipc_load(buf, &k_fixture_get_service_fsp);
  CHECK_OK(vmm_write_block(env->emu.vmm, env->emu.process.main_thread_tls_gva, buf, sizeof(buf)));

  const uint64_t tls_page = env->emu.process.main_thread_tls_gva & ~(uint64_t)(PAGE - 1u);
  CHECK_OK(vmm_reprotect(env->emu.vmm, tls_page, PAGE, VMM_PERM_R));
  env->regs->x[0] = sm_handle;
  svc(env, SVC_SEND_SYNC_REQUEST);
  CHECK(env->regs->x[0] == EXPECT_KERNEL_INVALID_ADDRESS);
  CHECK(sessions_in_use(&env->emu) == 1); /* only the sm: session */
  CHECK_OK(vmm_reprotect(env->emu.vmm, tls_page, PAGE, VMM_PERM_RW));

  CHECK(close_handle(env, sm_handle) == HLE_RESULT_SUCCESS);
}

/* --- Program lifecycle --- */

static void test_sessions_do_not_survive_unload(Test_Env *env, const Byte_Source *nca) {
  /* Program A leaves a session open; program B must not be able to use
   * it, and repeated cycles must not fill the table. */
  for (uint32_t cycle = 0; cycle < LOAD_CYCLES; cycle++) {
    const uint32_t leaked = connect_sm(env);
    emulator_unload_program(&env->emu);
    CHECK(sessions_in_use(&env->emu) == 0);
    load(env, nca);
    CHECK(send_fixture(env, leaked, &k_fixture_register_client) == EXPECT_KERNEL_INVALID_HANDLE);
  }
}

int main(void) {
  Fixture_Buffer nca_image = build_program_nca();
  const Byte_Source nca = byte_source_from_memory(nca_image.bytes, nca_image.size);

  Test_Env env;
  CHECK_OK(emulator_create(&env.emu));
  env.regs = env.emu.cpu_backend->get_register_file(env.emu.cpu_state);
  load(&env, &nca);

  test_connect_to_named_port(&env);
  test_libnx_sm_session(&env);
  test_out_object_without_free_slot(&env);
  test_write_back_fault_closes_new_session(&env);
  test_sessions_do_not_survive_unload(&env, &nca);

  emulator_destroy(&env.emu);
  fixture_buffer_free(&nca_image);
  printf("[" CHECK_NAME "] all checks passed\n");
  return 0;
}
