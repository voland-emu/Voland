/**
 * core/hle/kernel/ipc unit test: HIPC+CMIF parse/write, ipc_dispatch's
 * bsearch, and the Phase 1 session table - all pure, no vmm/CPU backend
 * (ipc.h's design goal). Hand-builds raw command buffers the same way
 * ipc_response_write itself does, so a parse/write round trip here is
 * also a cross-check that the two agree on the wire format.
 */
#define CHECK_NAME "ipc_test"
#include "check.h"

#include "hle/hle.h"
#include "hle/kernel/ipc.h"

#include <string.h>

#define BUF_BYTES TLS_IPC_COMMAND_BUFFER_BYTES

static void write_u32_at(uint8_t *buf, uint32_t offset, uint32_t v) { memcpy(buf + offset, &v, 4); }
static void write_u64_at(uint8_t *buf, uint32_t offset, uint64_t v) { memcpy(buf + offset, &v, 8); }

/* Builds a request buffer matching ipc_request_parse's supported subset -
 * the same shape hle_svc_send_sync_request hands it in practice. */
static void build_request(uint8_t *buf, uint32_t type, bool has_special, bool send_pid,
                          uint64_t pid, uint32_t command_id, const void *payload,
                          uint32_t payload_size) {
  memset(buf, 0, BUF_BYTES);
  uint32_t offset = 8;
  uint32_t word1_extra = 0;
  if (has_special) {
    write_u32_at(buf, offset, send_pid ? 1u : 0u);
    offset += 4;
    if (send_pid) {
      write_u64_at(buf, offset, pid);
      offset += 8;
    }
    word1_extra = 1u << 31;
  }
  const uint32_t raw_offset = (offset + 15u) & ~15u;
  const uint32_t cmif_bytes = 16u + payload_size;
  write_u32_at(buf, raw_offset, 0x49434653u); /* "SFCI" */
  write_u32_at(buf, raw_offset + 4, 0);        /* version */
  write_u64_at(buf, raw_offset + 8, command_id);
  if (payload_size > 0) memcpy(buf + raw_offset + 16, payload, payload_size);

  write_u32_at(buf, 0, type);
  write_u32_at(buf, 4, ((cmif_bytes / 4u) & 0x3FFu) | word1_extra);
}

static void test_parse_basic_no_special_header(void) {
  uint8_t buf[BUF_BYTES];
  const uint8_t payload[8] = {'f', 's', 'p', '-', 's', 'r', 'v', 0};
  build_request(buf, IPC_MESSAGE_TYPE_REQUEST, false, false, 0, 1, payload, sizeof(payload));

  IPC_Request req;
  CHECK_OK(ipc_request_parse(buf, BUF_BYTES, &req));
  CHECK(req.command_id == 1);
  CHECK(!req.has_pid);
  CHECK(req.data_size == sizeof(payload));
  CHECK(memcmp(req.data, payload, sizeof(payload)) == 0);
}

static void test_parse_with_pid(void) {
  uint8_t buf[BUF_BYTES];
  build_request(buf, IPC_MESSAGE_TYPE_REQUEST, true, true, 0xDEADBEEFull, 0, NULL, 0);

  IPC_Request req;
  CHECK_OK(ipc_request_parse(buf, BUF_BYTES, &req));
  CHECK(req.command_id == 0);
  CHECK(req.has_pid);
  CHECK(req.pid == 0xDEADBEEFull);
  CHECK(req.data_size == 0);
}

static void test_parse_rejects_bad_buffer_size(void) {
  uint8_t buf[BUF_BYTES];
  build_request(buf, IPC_MESSAGE_TYPE_REQUEST, false, false, 0, 0, NULL, 0);
  IPC_Request req;
  CHECK_CODE(ipc_request_parse(buf, BUF_BYTES - 1, &req), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(ipc_request_parse(NULL, BUF_BYTES, &req), RESULT_INVALID_ARGUMENT);
}

static void test_parse_rejects_non_request_type(void) {
  uint8_t buf[BUF_BYTES];
  build_request(buf, IPC_MESSAGE_TYPE_CONTROL, false, false, 0, 0, NULL, 0);
  IPC_Request req;
  CHECK_CODE(ipc_request_parse(buf, BUF_BYTES, &req), RESULT_NOT_IMPLEMENTED);
}

static void test_parse_rejects_buffer_descriptors(void) {
  uint8_t buf[BUF_BYTES];
  build_request(buf, IPC_MESSAGE_TYPE_REQUEST, false, false, 0, 0, NULL, 0);
  /* Set num_a_descriptors = 1 (word0 bits 20-23) directly - the parser
   * must reject on the count alone, before reading any descriptor. */
  uint32_t word0;
  memcpy(&word0, buf, 4);
  word0 |= (1u << 20);
  memcpy(buf, &word0, 4);
  IPC_Request req;
  CHECK_CODE(ipc_request_parse(buf, BUF_BYTES, &req), RESULT_NOT_IMPLEMENTED);
}

static void test_parse_rejects_request_move_handles(void) {
  uint8_t buf[BUF_BYTES];
  build_request(buf, IPC_MESSAGE_TYPE_REQUEST, true, false, 0, 0, NULL, 0);
  /* Special header word sits right after word0/word1; set move_handle_count
   * (bits 5-8) to 1. */
  uint32_t special;
  memcpy(&special, buf + 8, 4);
  special |= (1u << 5);
  memcpy(buf + 8, &special, 4);
  IPC_Request req;
  CHECK_CODE(ipc_request_parse(buf, BUF_BYTES, &req), RESULT_NOT_IMPLEMENTED);
}

static void test_parse_rejects_bad_magic(void) {
  uint8_t buf[BUF_BYTES];
  build_request(buf, IPC_MESSAGE_TYPE_REQUEST, false, false, 0, 0, NULL, 0);
  /* No special header: raw data starts at align_up(8, 16) = 16, not 8 -
   * see build_request. */
  write_u32_at(buf, 16, 0xFFFFFFFFu); /* clobber the SFCI magic */
  IPC_Request req;
  CHECK_CODE(ipc_request_parse(buf, BUF_BYTES, &req), RESULT_NOT_IMPLEMENTED);
}

/* --- ipc_dispatch --- */

static HLE_Service_Result fake_cmd_a(Service_Object *self, const IPC_Request *req,
                                     IPC_Response *res) {
  (void)self;
  (void)req;
  (void)res;
  return HLE_RESULT_SUCCESS;
}

static HLE_Service_Result fake_cmd_b(Service_Object *self, const IPC_Request *req,
                                     IPC_Response *res) {
  (void)self;
  (void)req;
  ipc_response_add_move_object(res, (Service_Object){.interface = NULL, .instance = self});
  return HLE_RESULT_SUCCESS;
}

static const Service_Command fake_commands[] = {
    {0, fake_cmd_a, "A"},
    {5, fake_cmd_b, "B"},
};
static const Service_Interface fake_interface = {
    .name = "fake:", .commands = fake_commands, .command_count = 2};

static void test_dispatch_known_and_unknown_command(void) {
  Service_Object object = {.interface = &fake_interface, .instance = NULL};
  IPC_Request req;
  memset(&req, 0, sizeof(req));

  req.command_id = 5;
  IPC_Response res;
  ipc_dispatch(&object, &req, &res);
  CHECK((uint32_t)res.result == HLE_RESULT_SUCCESS);
  CHECK(res.out_object_count == 1);

  req.command_id = 3; /* not in fake_commands */
  ipc_dispatch(&object, &req, &res);
  CHECK((uint32_t)res.result == HLE_RESULT_NOT_IMPLEMENTED);
  CHECK(res.out_object_count == 0);
}

static void test_response_add_move_object_caps_at_max(void) {
  IPC_Response res;
  memset(&res, 0, sizeof(res));
  CHECK(ipc_response_add_move_object(&res, (Service_Object){0}));
  CHECK(res.out_object_count == IPC_MAX_OUT_OBJECTS);
  /* IPC_MAX_OUT_OBJECTS is 1 (ipc.h) - a second add must fail, not grow
   * past the array. */
  CHECK(!ipc_response_add_move_object(&res, (Service_Object){0}));
  CHECK(res.out_object_count == IPC_MAX_OUT_OBJECTS);
}

/* --- ipc_response_write --- */

static void test_response_write_no_handles(void) {
  uint8_t buf[BUF_BYTES];
  IPC_Response res;
  memset(&res, 0, sizeof(res));
  res.result = HLE_RESULT_SUCCESS;

  CHECK_OK(ipc_response_write(buf, BUF_BYTES, &res, NULL, 0));

  uint32_t word1;
  memcpy(&word1, buf + 4, 4);
  CHECK((word1 >> 31) == 0); /* no special header: no handles to carry */
  const uint32_t raw_offset = (8u + 15u) & ~15u;
  uint32_t magic;
  memcpy(&magic, buf + raw_offset, 4);
  CHECK(magic == 0x4F434653u); /* "SFCO" */
  uint64_t result64;
  memcpy(&result64, buf + raw_offset + 8, 8);
  CHECK(result64 == HLE_RESULT_SUCCESS);
}

static void test_response_write_one_handle(void) {
  uint8_t buf[BUF_BYTES];
  IPC_Response res;
  memset(&res, 0, sizeof(res));
  res.result = HLE_RESULT_SUCCESS;
  res.out_object_count = 1;
  const uint32_t handle = 7;

  CHECK_OK(ipc_response_write(buf, BUF_BYTES, &res, &handle, 1));

  uint32_t word1;
  memcpy(&word1, buf + 4, 4);
  CHECK((word1 >> 31) == 1);
  uint32_t special;
  memcpy(&special, buf + 8, 4);
  CHECK(((special >> 5) & 0xFu) == 1); /* move_handle_count */
  uint32_t written_handle;
  memcpy(&written_handle, buf + 12, 4);
  CHECK(written_handle == handle);
}

static void test_response_write_rejects_handle_count_mismatch(void) {
  uint8_t buf[BUF_BYTES];
  IPC_Response res;
  memset(&res, 0, sizeof(res));
  res.out_object_count = 1;
  const uint32_t handle = 1;
  CHECK_CODE(ipc_response_write(buf, BUF_BYTES, &res, &handle, 0), RESULT_INVALID_ARGUMENT);
}

/* --- session table --- */

static void test_session_table_open_get_close(void) {
  IPC_Session_Table table;
  ipc_session_table_init(&table);

  const Service_Object object = {.interface = &fake_interface, .instance = (void *)0x1234};
  const uint32_t handle = ipc_session_table_open(&table, object);
  CHECK(handle != IPC_INVALID_HANDLE);

  IPC_Session *session = ipc_session_table_get(&table, handle);
  CHECK(session != NULL);
  CHECK(session->object.interface == &fake_interface);
  CHECK(session->object.instance == (void *)0x1234);
  CHECK(!session->is_domain);

  CHECK(ipc_session_table_get(&table, handle + 1000) == NULL);
  CHECK(ipc_session_table_get(&table, IPC_INVALID_HANDLE) == NULL);

  CHECK(ipc_session_table_close(&table, handle));
  CHECK(ipc_session_table_get(&table, handle) == NULL);
  CHECK(!ipc_session_table_close(&table, handle)); /* already closed */

  /* The freed slot is reused by the next open. */
  const uint32_t reopened = ipc_session_table_open(&table, object);
  CHECK(reopened == handle);
}

static void test_session_table_exhaustion(void) {
  IPC_Session_Table table;
  ipc_session_table_init(&table);
  const Service_Object object = {0};

  for (uint32_t i = 0; i < IPC_MAX_SESSIONS; i++) {
    CHECK(ipc_session_table_open(&table, object) != IPC_INVALID_HANDLE);
  }
  CHECK(ipc_session_table_open(&table, object) == IPC_INVALID_HANDLE);
}

int main(void) {
  test_parse_basic_no_special_header();
  test_parse_with_pid();
  test_parse_rejects_bad_buffer_size();
  test_parse_rejects_non_request_type();
  test_parse_rejects_buffer_descriptors();
  test_parse_rejects_request_move_handles();
  test_parse_rejects_bad_magic();
  test_dispatch_known_and_unknown_command();
  test_response_add_move_object_caps_at_max();
  test_response_write_no_handles();
  test_response_write_one_handle();
  test_response_write_rejects_handle_count_mismatch();
  test_session_table_open_get_close();
  test_session_table_exhaustion();
  printf("[" CHECK_NAME "] all checks passed\n");
  return 0;
}
